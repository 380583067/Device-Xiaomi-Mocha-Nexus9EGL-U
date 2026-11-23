/*
 * Copyright (C) CSR plc 2011
 * Copyright 2010, The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Author: marco.sinigaglia@csr.com for CSR plc
 */

#define LOG_TAG "V4l2_handler_Fm"
#define LOG_NDEBUG 1

#ifdef LINUX
#define ALOGI printf
#define ALOGE printf
#else
#include <log/log.h>
#endif

#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <ctype.h>
#include "../libfmjni/android_fm.h"
#include "v4l2_ioctl.h"

//RDS
#define RDS_THREAD_ON                    1
#define RDS_THREAD_OFF                   0
#define BUFFER_RDS_SIZE                  300                // rds buff size
#define GROUP_SIZE                       8                  // group is composed by 2byte * 4 packets
#define GROUP_EMPTY                      0
#define GROUP_INCOMPLETE                 1
#define GROUP_0A                         0
#define GROUP_0B                         1
#define GETBYTE(blockNuber,byteNum)      blockNuber*2+byteNum
#define CLEAN_RDS                        last_block_num= -1; next_expected_block= 0; group_status = GROUP_EMPTY;
//Mute
#define DEFAULT_VOLUME                  255
#define MUTE_OFF                        0
#define MUTE_ON                         1
//Scan
#define SCAN_RUN                        1
#define SCAN_STOP                       0
#define LOCKTIME                        40000           // wait 40ms for card to lock on
#define MAX_FREQS                       50              // number of max freq buffer for a full scan
#define DEFAULT_THRESHOLD               500             // threshold for scan
/* [auto-scan fix 2026-11-17] The whole band used to be judged against one fixed
 * threshold of 500. Measured here (fmtest --antenna plus scan logs): the noise
 * floor sits around 335-374 and the strongest real station (95.0 MHz) only
 * reaches 437, so it never crosses 500 and "refresh / auto scan" finds no station
 * at all, while tuning to one by hand plays fine.
 * It is adaptive now: measure the whole band first, use the mean as the noise
 * floor and "mean + half the peak height" as the threshold, then pick local
 * peaks. With a flat band (no headphone antenna) it falls back to the configured
 * threshold and keeps the old behaviour. */
#define SCAN_MAX_POINTS                 256             // max samples over the band (87.5-108 MHz / 100 kHz = 206)
#define SCAN_MIN_ABS                    250             // lower bound of the adaptive threshold (0-1000 scale)
#define SCAN_MIN_SPAN                   30              // a peak height below this means the band is flat (no station)
#define SCAN_PEAK_SEP                   250             // merge distance between stations (kHz, standard tuning step)
#define SCAN_NOISE_PRIME                4               // samples used to prime the noise floor on seek

/* session struct holded by the FM SE stack */
typedef struct fm_v4l2_data_t {
  int low_freq;
  int high_freq;
  int grid;
  int fd;
  int threshold;
  int freq;
  float fact;
  struct v4l2_tuner vt;
  pthread_t thread_rds;                                 /* thread used to read rds data */
  char scan_band_run;                                   /* flag to stop the scan*/
  char thread_rds_run;                                  /* flag to stop the rds thread*/
} fm_v4l2_data;


fm_v4l2_data* get_session_data(void **data) {return *data;}
int get_standard_freq(int freq, int fact) {return freq / fact;}
int get_proprietary_freq(int freq, int fact) {return freq * fact;}

void* th_read_rds(void *thread_rds_info);

static int v4l2_rx_start_func (void **data, int low_freq, int high_freq, int default_freq, int grid)
{
  char	*dev = DEFAULT_DEVICE;
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  ALOGI("low_freq %d, high_freq %d, default_freq %d, grid %d\n", low_freq, high_freq, default_freq, grid);

  session = malloc(sizeof(fm_v4l2_data));
  if (session== NULL){
      ALOGE("error on malloc");
      return -1;
  }

  memset(session, 0, sizeof(fm_v4l2_data));
  *data =  session;

  session->fd = open_dev(dev);
  if (session->fd < 0){
      ALOGE("error on open dev\n");
      return -1;
  }

  if (get_tun_radio_cap(session->fd) !=1) {
      ALOGE("error on check tunner radio capability");
      return -1;
  }

  session->vt.index=0;
  if ( get_v4l2_tuner(session->fd,  &session->vt) <0 ){
      ALOGE("error on get V4L tuner\n");
      return -1;
  }

  session->fact = get_fact(session->fd, &session->vt);
  if ( session->fact < 0) {
      ALOGE("error on get fact\n");
      return -1;
  }

  session->freq = get_proprietary_freq(default_freq, session->fact);
  session->low_freq =  get_proprietary_freq(low_freq,  session->fact);
  session->high_freq =  get_proprietary_freq(high_freq,  session->fact);
  session->grid = get_proprietary_freq(grid,  session->fact);
  session->threshold = DEFAULT_THRESHOLD;
  
  if (set_freq(session->fd, session->freq) < 0 ){
      ALOGE("error on set freq\n");
      return -1;
  }

  if (set_volume(session->fd, DEFAULT_VOLUME)<0){
      ALOGE("error on set volume\n");
      return -1;
  }

  if (set_mute(session->fd, MUTE_OFF) <0){
      ALOGE("error on mute\n");
      return -1;
  }

  return 0;
}

int v4l2_reset(void** session_data)
{
  fm_v4l2_data* session;
  int ret;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  ret = set_mute(session->fd, MUTE_ON);
  if (ret < 0)
    return -1;

  ret = close(session->fd);
  if (ret < 0)
    return -1;

  free(session);
  return 0;
}

int v4l2_pause(void** session_data){
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  return set_mute(session->fd, MUTE_ON);
}

int v4l2_resume(void** session_data){
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  return set_mute(session->fd, MUTE_OFF);
}

int v4l2_mute(void** session_data, int mute){
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  return set_mute(session->fd, mute);
}


int v4l2_set_frequency(void** session_data, int frequency){
  fm_v4l2_data* session;
  int ret;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  session->freq = get_proprietary_freq( frequency, session->fact);
  ret= set_freq(session->fd,  session->freq);
  if (ret < 0)
      return -1;

  return frequency;
}

int v4l2_get_frequency (void ** session_data){
  fm_v4l2_data* session;
  int ret;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  ret = get_freq(session->fd);
  if (ret < 0)
      return -1;

  session->freq = get_standard_freq(ret, session->fact);

  return session->freq;
}

int v4l2_get_threshold (void ** session_data){
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  return session->threshold;
 }

int v4l2_set_threshold (void ** session_data, int threshold){
   fm_v4l2_data* session;

   ALOGI("%s:\n", __FUNCTION__);
   session = get_session_data(session_data);

   session->threshold = threshold;
   return 0;
}

int v4l2_get_signal_strength (void ** session_data){
    fm_v4l2_data* session;
    int ret;

    ALOGI("%s:\n", __FUNCTION__);
    session = get_session_data(session_data);

    ret = get_signal_strength(session->fd, &session->vt);
    return ret;
}

int v4l2_is_playing_in_stereo (void ** session_data){
    fm_v4l2_data* session;
    int ret;

    ALOGI("%s:\n", __FUNCTION__);
    session = get_session_data(session_data);

    ret = get_stereo(session->fd, &session->vt);
    return ret;
}

int v4l2_scan (void ** session_data, enum fmradio_seek_direction_t direction){
   fm_v4l2_data* session;
   int increment, rate, freqi, ret;
   int noise = -1;          /* dynamic noise floor (0-1000 scale), -1 until estimated */
   int primed = 0;          /* samples already used for the noise floor */
   int dyn;                 /* current dynamic threshold */

   ALOGI("%s:\n", __FUNCTION__);
   session = get_session_data(session_data);

   session->scan_band_run=SCAN_RUN;
   if (direction==FMRADIO_SEEK_DOWN)
     increment = -  session->grid;
   else                                      //FMRADIO_SEEK_UP
     increment =  session->grid;

   freqi = session->freq + increment;         //drop the current frequency
   ALOGI("Starting scanning...\n");
   while (session->scan_band_run==SCAN_RUN){

      ret= set_freq(session->fd, freqi);
      if (ret<0)
          return -1;

      usleep(LOCKTIME);                       // let it lock on
      rate= get_signal_strength( session->fd,  &session->vt);
      if (rate < 0)
         return -1;

      /* [next/previous station fix] The old code compared against a fixed 500 and the
       * strongest station here only reaches about 437, so a single press never found
       * anything. The noise floor is estimated while scanning now: the minimum of the
       * first SCAN_NOISE_PRIME samples gives the floor (the start may sit on a strong
       * station, and the minimum is more robust than the first sample), then the
       * threshold is "noise floor + 1/8 of the remaining headroom". A hit is measured
       * once more to reject noise spikes. */
      if (primed < SCAN_NOISE_PRIME) {
          if (noise < 0 || rate < noise)
              noise = rate;
          primed++;
          ALOGI("seek prime %d/%d rate %d -> noise %d", primed, SCAN_NOISE_PRIME, rate, noise);
      } else {
          dyn = noise + (1000 - noise) / 8;
          if (dyn < SCAN_MIN_ABS)
              dyn = SCAN_MIN_ABS;
          ALOGI("seek %d: rate %d dyn %d noise %d", freqi, rate, dyn, noise);

          if (rate > dyn) {
              int confirm;
              usleep(LOCKTIME);
              confirm = get_signal_strength(session->fd, &session->vt);
              if (confirm < 0)
                  return -1;
              if (confirm > dyn) {
                  ALOGI("Found freq, %d (rate %d/%d > %d)\n", freqi, rate, confirm, dyn);
                  session->scan_band_run=SCAN_STOP;
                  session->freq = freqi;
                  return get_standard_freq(freqi, session->fact);
              }
          } else {
              /* update the floor only on non-candidate points, so a strong station cannot raise it */
              noise = (noise * 3 + rate) / 4;
          }
      }

      freqi +=  increment;
      if ( freqi >  session->high_freq)
         freqi =  session->low_freq;
      if ( freqi <  session->low_freq)
          freqi =  session->high_freq;
   }
   ALOGI("End scan\n");
   return 0;
}

int v4l2_full_scan (void ** session_data, int ** found_freqs, int ** signal_strenghts){
  fm_v4l2_data* session;
  int founded, i, ret, n;
  int freqi, rate;
  int *temp_freq, *temp_strenght;
  int all_freq[SCAN_MAX_POINTS];
  int all_rate[SCAN_MAX_POINTS];
  int total, mean, maxr, minr, span, thr;
  long long sum;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  session->scan_band_run=SCAN_RUN;

  total = (session->high_freq - session->low_freq) / session->grid + 1;
  if (total > SCAN_MAX_POINTS)
      total = SCAN_MAX_POINTS;
  if (total < 3)
      return 0;

  /* [auto-scan fix] First pass: measure the whole band once and cache it.
   * The old code compared against a fixed 500 while measuring, and the strongest
   * station here is 437, so it could never find one; only the full sweep gives a
   * real noise floor and peak height. */
  n = 0;
  for (freqi = session->low_freq;
       (freqi < session->high_freq) && (n < total) && (session->scan_band_run==SCAN_RUN);
       freqi += session->grid) {

      ret = set_freq(session->fd, freqi);
      if (ret<0)
         return -1;

      usleep(LOCKTIME);		/* let it lock on */
      rate= get_signal_strength(session->fd, &session->vt);
      if (rate < 0)
          return -1;

      all_freq[n] = freqi;
      all_rate[n] = rate;
      n++;
  }

  if (n < 3) {
      ALOGE("band too short (%d points)", n);
      session->scan_band_run=SCAN_STOP;
      return 0;
  }

  /* Noise floor and strongest point: the mean is the band floor, max - mean is the strongest peak height */
  sum = 0;
  maxr = all_rate[0];
  minr = all_rate[0];
  for (i = 0; i < n; i++) {
      sum += all_rate[i];
      if (all_rate[i] > maxr) maxr = all_rate[i];
      if (all_rate[i] < minr) minr = all_rate[i];
  }
  mean = (int)(sum / n);
  span = maxr - mean;

  if (span < SCAN_MIN_SPAN) {
      /* Flat band: either no headphone antenna or genuinely no station here. Fall
       * back to the user/default threshold, keeping the old "nothing found" semantics
       * so no fake stations are reported. */
      thr = session->threshold;
      ALOGI("band flat: noise %d min %d max %d span %d < %d, keep threshold %d",
            mean, minr, maxr, span, SCAN_MIN_SPAN, thr);
  } else {
      thr = mean + span / 2;
      if (thr < SCAN_MIN_ABS)
          thr = SCAN_MIN_ABS;
      ALOGI("adaptive threshold: noise %d max %d span %d -> %d",
            mean, maxr, span, thr);
  }

  temp_freq = (int *) malloc(sizeof(int) *MAX_FREQS);
  temp_strenght = (int *) malloc(sizeof(int) *MAX_FREQS);
  if (temp_freq == NULL || temp_strenght==NULL){
    ALOGE("error on allocate");
    return -1;
  }

  /* Second pass: keep only local peaks above the adaptive threshold and merge peaks
   * that are too close (one station spans one or two channels, keep the stronger). */
  founded = 0;
  for (i = 1; i < n - 1 && founded < MAX_FREQS; i++) {
      if (session->scan_band_run != SCAN_RUN)
          break;
      if (all_rate[i] < thr)
          continue;
      /* local peak test: not lower than either neighbour (a flat top counts) */
      if (all_rate[i] < all_rate[i-1] || all_rate[i] < all_rate[i+1])
          continue;

      if (founded > 0 &&
          (all_freq[i] - temp_freq[founded-1]) < get_proprietary_freq(SCAN_PEAK_SEP, session->fact)) {
          /* too close to the previous station: keep the stronger one only */
          if (all_rate[i] > temp_strenght[founded-1]) {
              temp_freq[founded-1] = all_freq[i];
              temp_strenght[founded-1] = all_rate[i];
          }
          continue;
      }

      ALOGI("peak index %d, freq %d, rate %d", founded, all_freq[i], all_rate[i]);
      temp_freq[founded] = all_freq[i];
      temp_strenght[founded] = all_rate[i];
      founded++;
  }

  ALOGI("band scan done: %d points, %d station(s), noise %d max %d thr %d",
        n, founded, mean, maxr, thr);

  *found_freqs = (int *) malloc(sizeof(int) * (founded > 0 ? founded : 1));
  *signal_strenghts = (int *) malloc(sizeof(int) * (founded > 0 ? founded : 1));
  if (*found_freqs == NULL || *signal_strenghts==NULL){
    ALOGE("error on allocate");
    free(temp_freq);
    free(temp_strenght);
    return -1;
  }

  //copy founded frequencies
  for (i=0; i<founded; i++){
      (*found_freqs)[i] = get_standard_freq(temp_freq[i], session->fact);
      (*signal_strenghts)[i] = temp_strenght[i];
      ALOGI("Copied index %d, freq %d, signal %d\n",i, (*found_freqs)[i], (*signal_strenghts)[i] );
  }

  ALOGI("End full scan\n");
  free(temp_freq);
  free(temp_strenght);
  session->scan_band_run=SCAN_STOP;

  return i;
}

int v4l2_stop_scan(void ** session_data){
  fm_v4l2_data* session;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);
  session->scan_band_run=SCAN_STOP;
  ALOGI("Stop scan value is %d\n", session->scan_band_run);

  return 1;
}

int v4l2_set_force_mono (void ** session_data, int force_mono){
  fm_v4l2_data* session;
  int ret;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  ret = set_force_mono(session->fd, &session->vt, force_mono);
  return ret;
}

int v4l2_is_rds_data_supported (void ** session_data){
  fm_v4l2_data* session;
  int ret;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  ret = get_RDS_cap(session->fd);
  return ret;
}

int v4l2_is_tuned_to_valid_channel (void ** session_data){
  fm_v4l2_data* session;
  int signal;

  ALOGI("%s:\n", __FUNCTION__);
  session = get_session_data(session_data);

  signal = get_signal_strength(session->fd, &session->vt);
  if ( signal > session->threshold )
    return 1;
  else
    return 0;
}

int v4l2_get_rds(void * * session_data, struct fmradio_rds_bundle_t * fmradio_rds_bundle) {
  int ret = 1;
  int bytesNum, i, blocknum, last_block_num, next_expected_block, group_status, group_type;
  char * buf, * group;
  char b0, b1, b2, b1_l5;

  int index;
  fm_v4l2_data * session = get_session_data(session_data);

  buf = malloc(sizeof(char) * BUFFER_RDS_SIZE);
  group = malloc(sizeof(char) * GROUP_SIZE);
  CLEAN_RDS

  bytesNum = read(session -> fd, buf, BUFFER_RDS_SIZE);
  if (bytesNum == -1 && errno == EINTR) {
    ALOGE("Error on RDS read\n");
    ret = 2;
  }


  if (bytesNum > 0) {
    ALOGI("read num bytes %d\n", bytesNum); //assuming to have always multiple of 3

    for (i = 0; i < bytesNum; i += 3) {
      b0 = buf[i];
      b1 = buf[i + 1];
      b2 = buf[i + 2];

      if ((b2 & 0x80) != 0) {
        CLEAN_RDS
        continue;
      }

      blocknum = b2 & 0x07; // What's the differnce between "Received Offset"

      if (blocknum == 4) blocknum = 2; // Treat C' as C
      if ((blocknum == 5) || (blocknum == 6)) continue; // ignore E Blocks

      if (blocknum == 7) { //invalid block
        CLEAN_RDS
        continue;
      }

      if (blocknum == last_block_num) continue;

      if ((group_status == GROUP_EMPTY) && (blocknum != 0)) continue;

      if (blocknum != next_expected_block) {
        CLEAN_RDS
        continue;
      }

      if (blocknum == 1) {
        group_type = (b1 >> 3);
      }

      group[2 * blocknum] = b0;
      group[2 * blocknum + 1] = b1;
      group_status = GROUP_INCOMPLETE;

      last_block_num = blocknum;
      next_expected_block = blocknum + 1;

      if (next_expected_block <= 3) continue;

      // PI code in block 0:
      fmradio_rds_bundle->pi = (group[GETBYTE(0, 1)] << 8) | group[GETBYTE(0, 0)];

      //some other info common in all groups:
      fmradio_rds_bundle->tp = group[GETBYTE(1, 1)] & 0x04;
      fmradio_rds_bundle->pty = (((group[GETBYTE(1, 1)] << 3) & 0x18) | ((group[GETBYTE(1, 0)] >> 5) & 0x07));
      // special
      b1_l5 = (group[GETBYTE(1, 0)] & 0x1F);

      switch (group_type) {
      case GROUP_0A:
      case GROUP_0B:

        fmradio_rds_bundle->ta = (b1_l5 & 0x10);
        fmradio_rds_bundle->ms = (b1_l5 & 0x08);

        index = (b1_l5 & 0x03) << 1;

        if (isprint(group[GETBYTE(3, 1)]) && isprint(group[GETBYTE(3, 0)])) {
          fmradio_rds_bundle->psn[index] = group[GETBYTE(3, 1)];
          fmradio_rds_bundle->psn[index + 1] = group[GETBYTE(3, 0)];
        } else {
          ret = 3;
          break;
        }

        if (index == 6) {
          fmradio_rds_bundle->psn[8] = '\0';
          ALOGI("Event Rds called!\n");
          ret = 4;
        } else {
          ret = 0;
        }
        break;
      }
      CLEAN_RDS
    }
  }

  free(buf);
  free(group);

  return ret;
}

int register_fmradio_functions(long *signature, struct fmradio_vendor_methods_t *vendor_methods)
{
    memset(vendor_methods, 0, sizeof(vendor_methods));

    vendor_methods->set_frequency = v4l2_set_frequency;
    vendor_methods->get_frequency = v4l2_get_frequency;
    vendor_methods->get_threshold = v4l2_get_threshold;
    vendor_methods->set_threshold = v4l2_set_threshold;
    vendor_methods->is_rds_data_supported = v4l2_is_rds_data_supported;
    vendor_methods->scan = v4l2_scan;
    vendor_methods->is_tuned_to_valid_channel = v4l2_is_tuned_to_valid_channel;
    vendor_methods->full_scan = v4l2_full_scan;
    vendor_methods->stop_scan = v4l2_stop_scan;
    vendor_methods->is_playing_in_stereo = v4l2_is_playing_in_stereo;
    vendor_methods->get_signal_strength = v4l2_get_signal_strength;
    vendor_methods->rx_start = v4l2_rx_start_func;
    vendor_methods->pause=v4l2_pause;
    vendor_methods->resume=v4l2_resume;
    vendor_methods->reset=v4l2_reset;
    vendor_methods->set_force_mono=v4l2_set_force_mono;
    vendor_methods->mute=v4l2_mute;
    vendor_methods->get_rds=v4l2_get_rds;

    *signature = FMRADIO_SIGNATURE;
    return 0;
}
