/*
 *  TV Input - Linux DVB interface
 *  Copyright (C) 2007 Andreas �man
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <errno.h>

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

#include <linux/dvb/frontend.h>
#include <linux/dvb/dmx.h>

#include <libhts/htscfg.h>
#include <syslog.h>

#include "tvhead.h"
#include "dispatch.h"
#include "dvb.h"
#include "dvb_support.h"
#include "diseqc.h"
#include "notify.h"

typedef struct dvb_fe_cmd {
  TAILQ_ENTRY(dvb_fe_cmd) link;
  th_dvb_mux_instance_t *tdmi;
} dvb_fe_cmd_t;


/**
 * On some cards the FEC readout, tuning and such things takes a very long
 * time (~0.5 s). Therefore we need to do the tuning and monitoring in a
 * separate thread
 */
static void *
dvb_fe_manager(void *aux)
{
  th_dvb_adapter_t *tda = aux;
  struct timespec ts;
  dvb_fe_cmd_t *c;
  int i, v;
  th_dvb_mux_instance_t *tdmi = NULL;
  fe_status_t fe_status;
  th_dvb_table_t *tdt;
  struct dvb_frontend_parameters p;
  char buf[100];

  while(1) {
    ts.tv_sec = time(NULL) + 1;
    ts.tv_nsec = 0;
    
    pthread_mutex_lock(&tda->tda_lock);
    pthread_cond_timedwait(&tda->tda_cond, &tda->tda_lock, &ts);
    c = TAILQ_FIRST(&tda->tda_fe_cmd_queue);
    if(c != NULL) {

      if(tdmi != NULL)
	dvb_mux_unref(tdmi);
      
      TAILQ_REMOVE(&tda->tda_fe_cmd_queue, c, link);
    }


    if(c != NULL) {

      /* Switch to a new mux */

      tdmi = c->tdmi;

      if(tdmi->tdmi_refcnt == 1) {
	dvb_mux_unref(tdmi);
	tdmi = NULL;
	pthread_mutex_unlock(&tda->tda_lock);
	continue;
      }
 
      pthread_mutex_unlock(&tda->tda_lock);

      p = tdmi->tdmi_fe_params;

      if(tda->tda_type == FE_QPSK) {
	/* DVB-S */
	int lowfreq, hifreq, switchfreq, hiband;

	lowfreq    = atoi(config_get_str("lnb_lowfreq",    "9750000" ));
	hifreq     = atoi(config_get_str("lnb_hifreq",     "10600000"));
	switchfreq = atoi(config_get_str("lnb_switchfreq", "11700000"));

	hiband = switchfreq && p.frequency > switchfreq;
	
	diseqc_setup(tda->tda_fe_fd,
		     0, /* switch position */
		     tdmi->tdmi_polarisation == POLARISATION_HORIZONTAL,
		     hiband);

	usleep(50000);

	if(hiband)
	  p.frequency = abs(p.frequency - hifreq);
	else
	  p.frequency = abs(p.frequency - lowfreq);
      }
 
      i = ioctl(tda->tda_fe_fd, FE_SET_FRONTEND, &p);
      if(i != 0) {
	dvb_mux_nicename(buf, sizeof(buf), tdmi);
	syslog(LOG_ERR, "\"%s\" tuning to \"%s\""
	       " -- Front configuration failed -- %s",
	       tda->tda_rootpath, buf, strerror(errno));
      }
      free(c);

      time(&tdmi->tdmi_got_adapter);

      /* Now that we have tuned, start demuxing of tables */

      pthread_mutex_lock(&tdmi->tdmi_table_lock);
      LIST_FOREACH(tdt, &tdmi->tdmi_tables, tdt_link) {
	if(tdt->tdt_fparams == NULL)
	  continue;

	ioctl(tdt->tdt_fd, DMX_SET_FILTER, tdt->tdt_fparams);
	free(tdt->tdt_fparams);
	tdt->tdt_fparams = NULL;
      }

      pthread_mutex_unlock(&tdmi->tdmi_table_lock);

      /* Allow tuning to settle */
      sleep(1);

      /* Reset FEC counter */
      ioctl(tda->tda_fe_fd, FE_READ_UNCORRECTED_BLOCKS, &v);
    }

    pthread_mutex_unlock(&tda->tda_lock);

    if(tdmi == NULL)
      continue;

    fe_status = 0;
    ioctl(tda->tda_fe_fd, FE_READ_STATUS, &fe_status);
    
    if(fe_status & FE_HAS_LOCK) {
      tdmi->tdmi_status = NULL;
    } else if(fe_status & FE_HAS_SYNC)
      tdmi->tdmi_status = "No lock, Sync Ok";
    else if(fe_status & FE_HAS_VITERBI)
      tdmi->tdmi_status = "No lock, FEC stable";
    else if(fe_status & FE_HAS_CARRIER)
      tdmi->tdmi_status = "Carrier only";
    else if(fe_status & FE_HAS_SIGNAL)
      tdmi->tdmi_status = "Faint signal";
    else
      tdmi->tdmi_status = "No signal";

    ioctl(tda->tda_fe_fd, FE_READ_UNCORRECTED_BLOCKS, &v);
    if(v < 0)
      v = 0;

    if(fe_status & FE_HAS_LOCK) {
      tdmi->tdmi_fec_err_histogram[tdmi->tdmi_fec_err_ptr] = v;
      tdmi->tdmi_fec_err_ptr++;
      if(tdmi->tdmi_fec_err_ptr == TDMI_FEC_ERR_HISTOGRAM_SIZE)
      tdmi->tdmi_fec_err_ptr = 0;
    }
  }
}


/**
 * Startup the FE management thread
 */
void
dvb_fe_start(th_dvb_adapter_t *tda)
{
  pthread_t ptid;
  pthread_create(&ptid, NULL, dvb_fe_manager, tda);
}


/**
 * Stop the given TDMI
 */
void
tdmi_stop(th_dvb_mux_instance_t *tdmi)
{
  th_dvb_table_t *tdt;

  tdmi->tdmi_adapter->tda_mux_current = NULL;

  pthread_mutex_lock(&tdmi->tdmi_table_lock);

  while((tdt = LIST_FIRST(&tdmi->tdmi_tables)) != NULL)
    dvb_tdt_destroy(tdt);

  pthread_mutex_unlock(&tdmi->tdmi_table_lock);

  tdmi->tdmi_state = TDMI_IDLE;
//  notify_tdmi_state_change(tdmi);
  time(&tdmi->tdmi_lost_adapter);
}


/**
 * Tune an adapter to a mux instance (but only if needed)
 */
void
dvb_tune_tdmi(th_dvb_mux_instance_t *tdmi, int maylog, tdmi_state_t state)
{
  dvb_fe_cmd_t *c;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;
  char buf[100];

  if(tdmi->tdmi_state != state) {
    tdmi->tdmi_state = state;
//    notify_tdmi_state_change(tdmi);
  }

  if(tda->tda_mux_current == tdmi)
    return;

  if(tda->tda_mux_current != NULL)
    tdmi_stop(tda->tda_mux_current);

  tda->tda_mux_current = tdmi;

  if(maylog) {
    dvb_mux_nicename(buf, sizeof(buf), tdmi);
    tvhlog(LOG_DEBUG, "dvb",
	   "\"%s\" tuning to mux \"%s\"", tda->tda_rootpath, buf);
  }
  /* Add tables which will be activated once the tuning is completed */

  dvb_table_add_default(tdmi);

  /* Send command to the thread */

  c = malloc(sizeof(dvb_fe_cmd_t));
  c->tdmi = tdmi;
  pthread_mutex_lock(&tda->tda_lock);
  tdmi->tdmi_refcnt++;
  TAILQ_INSERT_TAIL(&tda->tda_fe_cmd_queue, c, link);
  pthread_cond_signal(&tda->tda_cond);
  pthread_mutex_unlock(&tda->tda_lock);
}


/**
 * Flush pending tuning commands for frontend
 *
 * tda_lock must be held
 */
void
dvb_fe_flush(th_dvb_mux_instance_t *tdmi)
{
  dvb_fe_cmd_t *c;
  th_dvb_adapter_t *tda = tdmi->tdmi_adapter;

  TAILQ_FOREACH(c, &tda->tda_fe_cmd_queue, link) 
    if(c->tdmi == tdmi)
      break;
  if(c == NULL)
    return;

  TAILQ_REMOVE(&tda->tda_fe_cmd_queue, c, link);
  dvb_mux_unref(tdmi);
  free(c);
}