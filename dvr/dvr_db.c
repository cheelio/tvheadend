/*
 *  Digital Video Recorder
 *  Copyright (C) 2008 Andreas Öman
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
#include <assert.h>
#include <string.h>

#include <libhts/htssettings.h>

#include "tvhead.h"
#include "dvr.h"
#include "notify.h"

char *dvr_storage;
char *dvr_format;
char *dvr_file_postfix;


static int de_tally;

static int dvr_retention_time = 86400 * 31;

struct dvr_entry_list dvrentries;

static void dvr_save(dvr_entry_t *de);

static void dvr_timer_expire(void *aux);
static void dvr_timer_start_recording(void *aux);



/**
 *
 */
static void
dvrdb_changed(void)
{
  htsmsg_t *m = htsmsg_create();
  htsmsg_add_u32(m, "reload", 1);
  notify_by_msg("dvrdb", m);
}


/**
 *
 */
static void
dvr_entry_link(dvr_entry_t *de)
{
  time_t now, preamble;

  de->de_refcnt = 1;

  LIST_INSERT_HEAD(&dvrentries, de, de_global_link);

  time(&now);

  preamble = de->de_start - 30;

  if(now >= de->de_stop || de->de_dont_reschedule) {
    de->de_sched_state = DVR_COMPLETED;
    gtimer_arm_abs(&de->de_timer, dvr_timer_expire, de, 
	       de->de_stop + dvr_retention_time);

  } else {
    de->de_sched_state = DVR_SCHEDULED;

    gtimer_arm_abs(&de->de_timer, dvr_timer_start_recording, de, preamble);
  }
}


/**
 *
 */
void
dvr_entry_create_by_event(event_t *e, const char *creator)
{
  dvr_entry_t *de;
  char tbuf[30];
  struct tm tm;

  if(e->e_channel == NULL || e->e_title == NULL)
    return;

  LIST_FOREACH(de, &e->e_channel->ch_dvrs, de_channel_link)
    if(de->de_start == e->e_start && de->de_sched_state != DVR_COMPLETED)
      return;

  de = calloc(1, sizeof(dvr_entry_t));
  de->de_id = ++de_tally;

  de->de_channel = e->e_channel;
  LIST_INSERT_HEAD(&de->de_channel->ch_dvrs, de, de_channel_link);

  de->de_start   = e->e_start;
  de->de_stop    = e->e_start + e->e_duration;

  de->de_creator = strdup(creator);
  de->de_title   = strdup(e->e_title);
  de->de_desc    = e->e_desc  ? strdup(e->e_desc)  : NULL;

  dvr_entry_link(de);

  localtime_r(&de->de_start, &tm);
  strftime(tbuf, sizeof(tbuf), "%c", &tm);

  tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\" starting at %s, "
	 "scheduled for recording by \"%s\"",
	 de->de_title, de->de_channel->ch_name, tbuf, creator);
	 
  dvrdb_changed();
  dvr_save(de);
}


/**
 *
 */
void
dvr_entry_dec_ref(dvr_entry_t *de)
{
  lock_assert(&global_lock);

  if(de->de_refcnt > 1) {
    de->de_refcnt--;
    return;
  }

  free(de->de_creator);
  free(de->de_title);
  free(de->de_desc);

  free(de);
}




/**
 *
 */
static void
dvr_entry_remove(dvr_entry_t *de)
{
  hts_settings_remove("dvrdb/%d", de->de_id);

  gtimer_disarm(&de->de_timer);

  LIST_REMOVE(de, de_channel_link);
  LIST_REMOVE(de, de_global_link);

  dvrdb_changed();

  dvr_entry_dec_ref(de);
}


/**
 *
 */
static void
dvr_db_load_one(htsmsg_t *c, int id)
{
  dvr_entry_t *de;
  const char *s, *title, *creator;
  channel_t *ch;
  uint32_t start, stop;

  if(htsmsg_get_u32(c, "start", &start))
    return;
  if(htsmsg_get_u32(c, "stop", &stop))
    return;

  if((s = htsmsg_get_str(c, "channel")) == NULL)
    return;
  if((ch = channel_find_by_name(s, 0)) == NULL)
    return;

  if((title = htsmsg_get_str(c, "title")) == NULL)
    return;

  if((creator = htsmsg_get_str(c, "creator")) == NULL)
    return;

  de = calloc(1, sizeof(dvr_entry_t));
  de->de_id = id;

  de_tally = MAX(id, de_tally);

  de->de_channel = ch;
  LIST_INSERT_HEAD(&de->de_channel->ch_dvrs, de, de_channel_link);

  de->de_start   = start;
  de->de_stop    = stop;
  de->de_creator = strdup(creator);
  de->de_title   = strdup(title);
  
  tvh_str_set(&de->de_desc,     htsmsg_get_str(c, "description"));
  tvh_str_set(&de->de_filename, htsmsg_get_str(c, "filename"));
  tvh_str_set(&de->de_error,    htsmsg_get_str(c, "error"));

  htsmsg_get_u32(c, "noresched", &de->de_dont_reschedule);
  dvr_entry_link(de);
}


/**
 *
 */
static void
dvr_db_load(void)
{
  htsmsg_t *l, *c;
  htsmsg_field_t *f;

  if((l = hts_settings_load("dvrdb")) != NULL) {
    HTSMSG_FOREACH(f, l) {
      if((c = htsmsg_get_msg_by_field(f)) == NULL)
	continue;
      dvr_db_load_one(c, atoi(f->hmf_name));
    }
  }
}



/**
 *
 */
static void
dvr_save(dvr_entry_t *de)
{
  htsmsg_t *m = htsmsg_create();

  lock_assert(&global_lock);

  htsmsg_add_str(m, "channel", de->de_channel->ch_name);
  htsmsg_add_u32(m, "start", de->de_start);
  htsmsg_add_u32(m, "stop", de->de_stop);
  
  htsmsg_add_str(m, "creator", de->de_creator);

  if(de->de_filename != NULL)
    htsmsg_add_str(m, "filename", de->de_filename);

  htsmsg_add_str(m, "title", de->de_title);

  if(de->de_desc != NULL)
    htsmsg_add_str(m, "description", de->de_desc);


  if(de->de_error != NULL)
    htsmsg_add_str(m, "error", de->de_error);

  htsmsg_add_u32(m, "noresched", de->de_dont_reschedule);

  hts_settings_save(m, "dvrdb/%d", de->de_id);
  htsmsg_destroy(m);
}


/**
 *
 */
static void
dvr_timer_expire(void *aux)
{
  dvr_entry_t *de = aux;
  dvr_entry_remove(de);
 
}


/**
 *
 */
static void
dvr_stop_recording(dvr_entry_t *de, const char *errmsg)
{
  dvr_rec_unsubscribe(de);

  de->de_sched_state = DVR_COMPLETED;
  tvh_str_set(&de->de_error, errmsg);

  tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\": "
	 "End of program: %s",
	 de->de_title, de->de_channel->ch_name,
	 de->de_error ?: "Program ended");

  dvrdb_changed();
  dvr_save(de);

  gtimer_arm_abs(&de->de_timer, dvr_timer_expire, de, 
		 de->de_stop + dvr_retention_time);  
}



/**
 *
 */
static void
dvr_timer_stop_recording(void *aux)
{
  dvr_entry_t *de = aux;
  dvr_stop_recording(de, NULL);
}



/**
 *
 */
static void
dvr_timer_start_recording(void *aux)
{
  dvr_entry_t *de = aux;

  de->de_sched_state = DVR_RECORDING;

  tvhlog(LOG_INFO, "dvr", "\"%s\" on \"%s\" recorder starting",
	 de->de_title, de->de_channel->ch_name);

  dvrdb_changed();

  dvr_rec_subscribe(de);

  gtimer_arm_abs(&de->de_timer, dvr_timer_stop_recording, de, de->de_stop);
}


/**
 *
 */
dvr_entry_t *
dvr_entry_find_by_id(int id)
{
  dvr_entry_t *de;
  LIST_FOREACH(de, &dvrentries, de_global_link)
    if(de->de_id == id)
      break;
  return de;  
}

/**
 *
 */
void
dvr_entry_cancel(dvr_entry_t *de)
{
  switch(de->de_sched_state) {
  case DVR_SCHEDULED:
    dvr_entry_remove(de);
    break;

  case DVR_RECORDING:
    de->de_dont_reschedule = 1;
    dvr_stop_recording(de, "Aborted by user");
    break;

  case DVR_COMPLETED:
    dvr_entry_remove(de);
    break;
  }
}


/**
 *
 */
void
dvr_init(void)
{
  dvr_storage      = strdup("/home/andoma/media/dvr");
  dvr_format       = strdup("matroska");
  dvr_file_postfix = strdup("mkv");

  dvr_db_load();
}


/**
 *
 */
static void
dvr_query_add_entry(dvr_query_result_t *dqr, dvr_entry_t *de)
{
  if(dqr->dqr_entries == dqr->dqr_alloced) {
    /* Need to alloc more space */

    dqr->dqr_alloced = MAX(100, dqr->dqr_alloced * 2);
    dqr->dqr_array = realloc(dqr->dqr_array, 
			     dqr->dqr_alloced * sizeof(dvr_entry_t *));
  }
  dqr->dqr_array[dqr->dqr_entries++] = de;
}


/**
 *
 */
void
dvr_query(dvr_query_result_t *dqr)
{
  dvr_entry_t *de;

  memset(dqr, 0, sizeof(dvr_query_result_t));

  LIST_FOREACH(de, &dvrentries, de_global_link)
    dvr_query_add_entry(dqr, de);
}


/**
 *
 */
void
dvr_query_free(dvr_query_result_t *dqr)
{
  free(dqr->dqr_array);
}

/**
 * Sorting functions
 */
static int
dvr_sort_start_descending(const void *A, const void *B)
{
  dvr_entry_t *a = *(dvr_entry_t **)A;
  dvr_entry_t *b = *(dvr_entry_t **)B;
  return b->de_start - a->de_start;
}


/**
 *
 */
void
dvr_query_sort(dvr_query_result_t *dqr)
{
  int (*sf)(const void *a, const void *b);

  if(dqr->dqr_array == NULL)
    return;

  sf = dvr_sort_start_descending;
  qsort(dqr->dqr_array, dqr->dqr_entries, sizeof(dvr_entry_t *), sf);
}  
