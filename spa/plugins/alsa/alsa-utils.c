#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include <errno.h>
#include <getopt.h>
#include <sys/time.h>
#include <math.h>

#include "alsa-utils.h"

static int verbose = 0;					/* verbose flag */

#define CHECK(s,msg) if ((err = (s)) < 0) { printf (msg ": %s\n", snd_strerror(err)); return err; }

static int
spa_alsa_open (SpaALSAState *state)
{
  int err;
  SpaALSAProps *props = &state->props[1];

  if (state->opened)
    return 0;

  CHECK (snd_output_stdio_attach (&state->output, stderr, 0), "attach failed");

  printf ("ALSA device open '%s'\n", props->device);
  CHECK (snd_pcm_open (&state->hndl,
                       props->device,
                       state->stream,
                       SND_PCM_NONBLOCK |
                       SND_PCM_NO_AUTO_RESAMPLE |
                       SND_PCM_NO_AUTO_CHANNELS |
                       SND_PCM_NO_AUTO_FORMAT), "open failed");

  state->opened = true;

  return 0;
}

static int
spa_alsa_close (SpaALSAState *state)
{
  int err = 0;

  if (!state->opened)
    return 0;

  printf ("Playback device closing\n");
  CHECK (snd_pcm_close (state->hndl), "close failed");

  state->opened = false;

  return err;
}

static snd_pcm_format_t
spa_alsa_format_to_alsa (SpaAudioFormat format)
{
  switch (format) {
    case SPA_AUDIO_FORMAT_S8:
      return SND_PCM_FORMAT_S8;
    case SPA_AUDIO_FORMAT_U8:
      return SND_PCM_FORMAT_U8;
    /* 16 bit */
    case SPA_AUDIO_FORMAT_S16LE:
      return SND_PCM_FORMAT_S16_LE;
    case SPA_AUDIO_FORMAT_S16BE:
      return SND_PCM_FORMAT_S16_BE;
    case SPA_AUDIO_FORMAT_U16LE:
      return SND_PCM_FORMAT_U16_LE;
    case SPA_AUDIO_FORMAT_U16BE:
      return SND_PCM_FORMAT_U16_BE;
    /* 24 bit in low 3 bytes of 32 bits */
    case SPA_AUDIO_FORMAT_S24_32LE:
      return SND_PCM_FORMAT_S24_LE;
    case SPA_AUDIO_FORMAT_S24_32BE:
      return SND_PCM_FORMAT_S24_BE;
    case SPA_AUDIO_FORMAT_U24_32LE:
      return SND_PCM_FORMAT_U24_LE;
    case SPA_AUDIO_FORMAT_U24_32BE:
      return SND_PCM_FORMAT_U24_BE;
    /* 24 bit in 3 bytes */
    case SPA_AUDIO_FORMAT_S24LE:
      return SND_PCM_FORMAT_S24_3LE;
    case SPA_AUDIO_FORMAT_S24BE:
      return SND_PCM_FORMAT_S24_3BE;
    case SPA_AUDIO_FORMAT_U24LE:
      return SND_PCM_FORMAT_U24_3LE;
    case SPA_AUDIO_FORMAT_U24BE:
      return SND_PCM_FORMAT_U24_3BE;
    /* 32 bit */
    case SPA_AUDIO_FORMAT_S32LE:
      return SND_PCM_FORMAT_S32_LE;
    case SPA_AUDIO_FORMAT_S32BE:
      return SND_PCM_FORMAT_S32_BE;
    case SPA_AUDIO_FORMAT_U32LE:
      return SND_PCM_FORMAT_U32_LE;
    case SPA_AUDIO_FORMAT_U32BE:
      return SND_PCM_FORMAT_U32_BE;
    default:
      break;
  }

  return SND_PCM_FORMAT_UNKNOWN;
}

int
spa_alsa_set_format (SpaALSAState *state, SpaFormatAudio *fmt, bool try_only)
{
  unsigned int rrate;
  snd_pcm_uframes_t size;
  int err, dir;
  snd_pcm_hw_params_t *params;
  snd_pcm_format_t format;
  SpaAudioInfoRaw *info = &fmt->info.raw;
  snd_pcm_t *hndl;
  unsigned int buffer_time;
  unsigned int period_time;
  SpaALSAProps *props = &state->props[1];

  if ((err = spa_alsa_open (state)) < 0)
    return err;

  hndl = state->hndl;

  snd_pcm_hw_params_alloca (&params);
  /* choose all parameters */
  CHECK (snd_pcm_hw_params_any (hndl, params), "Broken configuration for playback: no configurations available");
  /* set hardware resampling */
  CHECK (snd_pcm_hw_params_set_rate_resample (hndl, params, 0), "set_rate_resample");
  /* set the interleaved read/write format */
  CHECK (snd_pcm_hw_params_set_access(hndl, params, SND_PCM_ACCESS_MMAP_INTERLEAVED), "set_access");

  /* set the sample format */
  format = spa_alsa_format_to_alsa (info->format);
  printf ("Stream parameters are %iHz, %s, %i channels\n", info->rate, snd_pcm_format_name(format), info->channels);
  CHECK (snd_pcm_hw_params_set_format (hndl, params, format), "set_format");
  /* set the count of channels */
  CHECK (snd_pcm_hw_params_set_channels (hndl, params, info->channels), "set_channels");
  /* set the stream rate */
  rrate = info->rate;
  CHECK (snd_pcm_hw_params_set_rate_near (hndl, params, &rrate, 0), "set_rate_near");
  if (rrate != info->rate) {
    printf("Rate doesn't match (requested %iHz, get %iHz)\n", info->rate, rrate);
    return -EINVAL;
  }

  state->frame_size = info->channels * 2;

  /* set the buffer time */
  buffer_time = props->buffer_time;
  CHECK (snd_pcm_hw_params_set_buffer_time_near (hndl, params, &buffer_time, &dir), "set_buffer_time_near");
  CHECK (snd_pcm_hw_params_get_buffer_size (params, &size), "get_buffer_size");
  state->buffer_frames = size;

  /* set the period time */
  period_time = props->period_time;
  CHECK (snd_pcm_hw_params_set_period_time_near (hndl, params, &period_time, &dir), "set_period_time_near");
  CHECK (snd_pcm_hw_params_get_period_size (params, &size, &dir), "get_period_size");
  state->period_frames = size;

  /* write the parameters to device */
  CHECK (snd_pcm_hw_params (hndl, params), "set_hw_params");

  return 0;
}

static int
set_swparams (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err = 0;
  snd_pcm_sw_params_t *params;
  SpaALSAProps *props = &state->props[1];

  snd_pcm_sw_params_alloca (&params);

  /* get the current params */
  CHECK (snd_pcm_sw_params_current (hndl, params), "sw_params_current");
  /* start the transfer when the buffer is almost full: */
  /* (buffer_frames / avail_min) * avail_min */
  CHECK (snd_pcm_sw_params_set_start_threshold (hndl, params,
        (state->buffer_frames / state->period_frames) * state->period_frames), "set_start_threshold");

  /* allow the transfer when at least period_size samples can be processed */
  /* or disable this mechanism when period event is enabled (aka interrupt like style processing) */
  CHECK (snd_pcm_sw_params_set_avail_min (hndl, params,
        props->period_event ? state->buffer_frames : state->period_frames), "set_avail_min");
  /* enable period events when requested */
  if (props->period_event) {
    CHECK (snd_pcm_sw_params_set_period_event (hndl, params, 1), "set_period_event");
  }
  /* write the parameters to the playback device */
  CHECK (snd_pcm_sw_params (hndl, params), "sw_params");

  return 0;
}

/*
 *   Underrun and suspend recovery
 */
static int
xrun_recovery (snd_pcm_t *hndl, int err)
{
  if (verbose)
    printf("stream recovery\n");
  if (err == -EPIPE) {	/* under-run */
    err = snd_pcm_prepare(hndl);
    if (err < 0)
      printf("Can't recovery from underrun, prepare failed: %s\n", snd_strerror(err));
    return 0;
  } else if (err == -ESTRPIPE) {
    while ((err = snd_pcm_resume(hndl)) == -EAGAIN)
      sleep(1);	/* wait until the suspend flag is released */
    if (err < 0) {
      err = snd_pcm_prepare(hndl);
      if (err < 0)
        printf("Can't recovery from suspend, prepare failed: %s\n", snd_strerror(err));
    }
    return 0;
  }
  return err;
}

static void
pull_input (SpaALSAState *state, void *data, snd_pcm_uframes_t frames)
{
  SpaNodeEvent event;
  SpaNodeEventNeedInput ni;

  event.type = SPA_NODE_EVENT_TYPE_NEED_INPUT;
  event.size = sizeof (ni);
  event.data = &ni;
  ni.port_id = 0;
  state->event_cb (&state->node, &event, state->user_data);
}

static int
mmap_write (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err;
  snd_pcm_sframes_t avail, commitres;
  snd_pcm_uframes_t offset, frames, size;
  const snd_pcm_channel_area_t *my_areas;

  if ((avail = snd_pcm_avail_update (hndl)) < 0) {
    if ((err = xrun_recovery (hndl, avail)) < 0) {
      printf ("Write error: %s\n", snd_strerror (err));
      return -1;
    }
  }

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      if ((err = xrun_recovery(hndl, err)) < 0) {
        printf("MMAP begin avail error: %s\n", snd_strerror(err));
        return -1;
      }
    }

    pull_input (state,
                (uint8_t *)my_areas[0].addr + (offset * sizeof (uint16_t) * 2),
                frames);

    commitres = snd_pcm_mmap_commit (hndl, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((err = xrun_recovery (hndl, commitres >= 0 ? -EPIPE : commitres)) < 0) {
        printf("MMAP commit error: %s\n", snd_strerror(err));
        return -1;
      }
    }
    size -= frames;
  }
  return 0;
}

static int
mmap_read (SpaALSAState *state)
{
  snd_pcm_t *hndl = state->hndl;
  int err;
  snd_pcm_sframes_t avail, commitres;
  snd_pcm_uframes_t offset, frames, size;
  snd_pcm_status_t *status;
  const snd_pcm_channel_area_t *my_areas;
  SpaALSABuffer *b;
  snd_htimestamp_t htstamp = { 0, 0 };
  int64_t now;
  uint8_t *dest;

  snd_pcm_status_alloca(&status);

  if ((err = snd_pcm_status (hndl, status)) < 0)
    return err;

  avail = snd_pcm_status_get_avail (status);
  snd_pcm_status_get_htstamp (status, &htstamp);
  now = (int64_t)htstamp.tv_sec * 1000000000ll + (int64_t)htstamp.tv_nsec;

  b = state->free_head;
  if (b == NULL)
    fprintf (stderr, "no more buffers\n");
  else {
    state->free_head = b->next;
    if (state->free_head == NULL)
      state->free_tail = NULL;
    b->next = NULL;
    dest = b->ptr;
  }

  size = avail;
  while (size > 0) {
    frames = size;
    if ((err = snd_pcm_mmap_begin (hndl, &my_areas, &offset, &frames)) < 0) {
      if ((err = xrun_recovery(hndl, err)) < 0) {
        printf("MMAP begin avail error: %s\n", snd_strerror (err));
        return -1;
      }
    }

    if (b) {
      size_t n_bytes = frames * state->frame_size;

      memcpy (dest,
              (uint8_t *)my_areas[0].addr + (offset * state->frame_size),
              n_bytes);
      dest += n_bytes;
    }

    commitres = snd_pcm_mmap_commit (hndl, offset, frames);
    if (commitres < 0 || (snd_pcm_uframes_t)commitres != frames) {
      if ((err = xrun_recovery (hndl, commitres >= 0 ? -EPIPE : commitres)) < 0) {
        printf("MMAP commit error: %s\n", snd_strerror(err));
        return -1;
      }
    }
    size -= frames;
  }

  if (b) {
    SpaNodeEvent event;
    SpaNodeEventHaveOutput ho;
    SpaData *d;

    d = SPA_BUFFER_DATAS (b->outbuf);
    d[0].mem.size = avail * state->frame_size;

    if (state->ready_tail)
      state->ready_tail->next = b;
    state->ready_tail = b;
    if (state->ready_head == NULL)
      state->ready_head = b;
    state->ready_count++;

    event.type = SPA_NODE_EVENT_TYPE_HAVE_OUTPUT;
    event.size = sizeof (ho);
    event.data = &ho;
    ho.port_id = 0;
    state->event_cb (&state->node, &event, state->user_data);
  }
  return 0;
}

static int
alsa_on_fd_events (SpaPollNotifyData *data)
{
  SpaALSAState *state = data->user_data;
  snd_pcm_t *hndl = state->hndl;
  int err;
  unsigned short revents;

  snd_pcm_poll_descriptors_revents (hndl,
                                    (struct pollfd *)data->fds,
                                    data->n_fds,
                                    &revents);
  if (revents & POLLERR) {
    if (snd_pcm_state (hndl) == SND_PCM_STATE_XRUN ||
        snd_pcm_state (hndl) == SND_PCM_STATE_SUSPENDED) {
      err = snd_pcm_state (hndl) == SND_PCM_STATE_XRUN ? -EPIPE : -ESTRPIPE;
      if ((err = xrun_recovery (hndl, err)) < 0) {
        printf ("error: %s\n", snd_strerror (err));
        return -1;
      }
    } else {
      printf("Wait for poll failed\n");
      return -1;
    }
  }

  if (state->stream == SND_PCM_STREAM_CAPTURE) {
    if (!(revents & POLLIN))
      return -1;

    mmap_read (state);
  } else {
    if (!(revents & POLLOUT))
      return -1;

    mmap_write (state);
  }

  return 0;
}

int
spa_alsa_start (SpaALSAState *state)
{
  int err;
  SpaNodeEvent event;

  if (spa_alsa_open (state) < 0)
    return -1;

  if (!state->have_buffers)
    return -1;

  CHECK (set_swparams (state), "swparams");

  snd_pcm_dump (state->hndl, state->output);

  if ((state->poll.n_fds = snd_pcm_poll_descriptors_count (state->hndl)) <= 0) {
    printf ("Invalid poll descriptors count\n");
    return state->poll.n_fds;
  }
  if ((err = snd_pcm_poll_descriptors (state->hndl, (struct pollfd *)state->fds, state->poll.n_fds)) < 0) {
    printf ("Unable to obtain poll descriptors for playback: %s\n", snd_strerror(err));
    return err;
  }

  event.type = SPA_NODE_EVENT_TYPE_ADD_POLL;
  event.data = &state->poll;
  event.size = sizeof (state->poll);

  state->poll.id = 0;
  state->poll.enabled = true;
  state->poll.fds = state->fds;
  state->poll.idle_cb = NULL;
  state->poll.before_cb = NULL;
  state->poll.after_cb = alsa_on_fd_events;
  state->poll.user_data = state;
  state->event_cb (&state->node, &event, state->user_data);

  mmap_write (state);
  err = snd_pcm_start (state->hndl);

  return err;
}

int
spa_alsa_stop (SpaALSAState *state)
{
  SpaNodeEvent event;

  if (!state->opened)
    return 0;

  snd_pcm_drop (state->hndl);

  event.type = SPA_NODE_EVENT_TYPE_REMOVE_POLL;
  event.data = &state->poll;
  event.size = sizeof (state->poll);
  state->event_cb (&state->node, &event, state->user_data);

  spa_alsa_close (state);

  return 0;
}