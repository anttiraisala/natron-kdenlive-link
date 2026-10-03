/* filter_check - drives the natron_link filter through the MLT API, like Kdenlive
 * would, and classifies every output frame by comparing it with an identical
 * producer that has no filter:
 *   inverted  = RGB inverted, alpha unchanged (what the mock worker does)
 *   unchanged = identical to the reference (pass-through)
 *   other     = anything else (a bug)
 * Prints one line:  check frames=N inverted=A unchanged=B other=C elapsed_ms=T
 *
 * Usage: filter_check [--frames N] [--mode auto|playback|export] [--real-time N]
 *                     [--consumer-service NAME]   (sets mlt_service on the fake consumer)
 *                     [--playback-timeout-ms T] [--export-timeout-ms T]
 *                     [--comp NAME] [--ntp FILE] [--start F] [--sleep-ms M]
 *                     [--clear-ntp] [--save-xml FILE]
 *                     [--open-at-start] [--clicks N] [--click-after-ms MS] [--filter-in N]
 * Then prints the composition properties of the filter:
 *   props ntp=... comp=... nkb_auto_comp=...
 * --clear-ntp   afterwards empties "ntp", renders one more frame and prints the props again
 * --filter-in N  sets the filter's in point (Kdenlive sets it to the clip's start)
 * --open-at-start  sets open_natron=1 right after creating the filter (like loading a
 *               project with that value saved; must NOT open Natron)
 * --clicks N    after the frames, waits --click-after-ms (default 1700) and then toggles
 *               open_natron N times, 300 ms apart, like clicks on the checkbox
 * --save-xml    finally saves the filtered producer with MLT's xml consumer (as Kdenlive
 *               saves a project) so a test can check which properties are stored
 */
#include <framework/mlt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static const char* arg(int argc, char** argv, const char* name, const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (!strcmp(argv[i], name)) return argv[i + 1];
  return def;
}

static void print_props(mlt_properties fp) {
  const char* ntp = mlt_properties_get(fp, "ntp");
  const char* comp = mlt_properties_get(fp, "comp");
  const char* own = mlt_properties_get(fp, "nkb_auto_comp");
  printf("props ntp=%s comp=%s nkb_auto_comp=%s\n", ntp ? ntp : "", comp ? comp : "", own ? own : "");
}

static int has_flag(int argc, char** argv, const char* name) {
  for (int i = 1; i < argc; ++i)
    if (!strcmp(argv[i], name)) return 1;
  return 0;
}

/* Renders one frame of the filtered producer (the extra frame for --clear-ntp). */
static int render_one(mlt_producer p, int pos, mlt_profile profile) {
  mlt_producer_seek(p, pos);
  mlt_frame frame = NULL;
  if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &frame, 0) || !frame) return 1;
  uint8_t* img;
  int w = profile->width, h = profile->height;  /* requested size: must be set before get_image */
  mlt_image_format fmt = mlt_image_rgba;
  const int err = mlt_frame_get_image(frame, &img, &fmt, &w, &h, 0);
  mlt_frame_close(frame);
  return err;
}

static mlt_producer make_color(mlt_profile profile, int frames) {
  mlt_producer p = mlt_factory_producer(profile, NULL, "color:#336699");
  if (!p) { fprintf(stderr, "cannot create color producer\n"); exit(2); }
  mlt_properties_set_int(MLT_PRODUCER_PROPERTIES(p), "length", frames + 100);
  mlt_producer_set_in_and_out(p, 0, frames + 99);
  return p;
}

static int get_rgba(mlt_producer p, int pos, uint8_t** img, int* w, int* h, mlt_frame* keep) {
  mlt_producer_seek(p, pos);
  mlt_frame frame = NULL;
  if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(p), &frame, 0) || !frame) return 1;
  mlt_image_format fmt = mlt_image_rgba;
  if (mlt_frame_get_image(frame, img, &fmt, w, h, 0) || fmt != mlt_image_rgba) return 1;
  *keep = frame;
  return 0;
}

int main(int argc, char** argv) {
  const int frames = atoi(arg(argc, argv, "--frames", "5"));
  const int start = atoi(arg(argc, argv, "--start", "0"));
  const int sleep_ms = atoi(arg(argc, argv, "--sleep-ms", "0"));
  mlt_factory_init(NULL);
  mlt_profile profile = mlt_profile_init(NULL);
  profile->width = 320;
  profile->height = 180;
  profile->display_aspect_num = 16;
  profile->display_aspect_den = 9;
  profile->sample_aspect_num = 1;
  profile->sample_aspect_den = 1;

  mlt_producer ref = make_color(profile, start + frames);
  mlt_producer filtered = make_color(profile, start + frames);
  mlt_filter f = mlt_factory_filter(profile, "natron_link", NULL);
  if (!f) { fprintf(stderr, "cannot create filter natron_link (module not found?)\n"); return 3; }
  mlt_properties fp = MLT_FILTER_PROPERTIES(f);
  mlt_properties_set(fp, "mode", arg(argc, argv, "--mode", "auto"));
  mlt_properties_set(fp, "comp", arg(argc, argv, "--comp", "filter_test"));
  mlt_properties_set(fp, "ntp", arg(argc, argv, "--ntp", ""));
  mlt_properties_set(fp, "playback_timeout_ms", arg(argc, argv, "--playback-timeout-ms", "250"));
  mlt_properties_set(fp, "export_timeout_ms", arg(argc, argv, "--export-timeout-ms", "5000"));
  if (*arg(argc, argv, "--filter-in", "")) mlt_properties_set(fp, "in", arg(argc, argv, "--filter-in", "0"));
  if (has_flag(argc, argv, "--open-at-start")) mlt_properties_set(fp, "open_natron", "1");
  mlt_service_attach(MLT_PRODUCER_SERVICE(filtered), f);

  /* A fake consumer property bag so mode=auto can read real_time, as it would from a real consumer. */
  mlt_properties fake_consumer = mlt_properties_new();
  mlt_properties_set_int(fake_consumer, "real_time", atoi(arg(argc, argv, "--real-time", "1")));
  const char* svc = arg(argc, argv, "--consumer-service", "");
  if (*svc) mlt_properties_set(fake_consumer, "mlt_service", svc);

  int inverted = 0, unchanged = 0, other = 0;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  for (int i = start; i < start + frames; ++i) {
    uint8_t *ri, *fi;
    /* mlt_frame_get_image reads width/height as the requested size, so they must be set. */
    int rw = profile->width, rh = profile->height, fw = profile->width, fh = profile->height;
    mlt_frame rframe, fframe;
    if (get_rgba(ref, i, &ri, &rw, &rh, &rframe)) { fprintf(stderr, "reference frame %d failed\n", i); return 4; }
    mlt_producer_seek(filtered, i);
    fframe = NULL;
    if (mlt_service_get_frame(MLT_PRODUCER_SERVICE(filtered), &fframe, 0) || !fframe) { fprintf(stderr, "frame %d failed\n", i); return 4; }
    mlt_properties_set_data(MLT_FRAME_PROPERTIES(fframe), "consumer", fake_consumer, 0, NULL, NULL);
    mlt_image_format fmt = mlt_image_rgba;
    if (mlt_frame_get_image(fframe, &fi, &fmt, &fw, &fh, 0) || fw != rw || fh != rh) { fprintf(stderr, "image %d failed\n", i); return 4; }

    int same = 1, inv = 1;
    for (int p = 0; p < rw * rh; ++p) {
      const uint8_t* a = ri + 4 * p;
      const uint8_t* b = fi + 4 * p;
      if (memcmp(a, b, 4)) same = 0;
      if ((uint8_t)(255 - a[0]) != b[0] || (uint8_t)(255 - a[1]) != b[1] || (uint8_t)(255 - a[2]) != b[2] || a[3] != b[3]) inv = 0;
    }
    if (same) unchanged++; else if (inv) inverted++; else other++;
    mlt_frame_close(rframe);
    mlt_frame_close(fframe);
    if (sleep_ms) usleep(sleep_ms * 1000);
  }
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
  printf("check frames=%d inverted=%d unchanged=%d other=%d elapsed_ms=%ld\n", frames, inverted, unchanged, other, ms);
  print_props(fp);
  if (has_flag(argc, argv, "--clear-ntp")) {
    mlt_properties_set(fp, "ntp", "");
    if (render_one(filtered, start, profile)) { fprintf(stderr, "extra frame failed\n"); return 4; }
    print_props(fp);
  }
  const int clicks = atoi(arg(argc, argv, "--clicks", "0"));
  if (clicks > 0) {
    usleep(atoi(arg(argc, argv, "--click-after-ms", "1700")) * 1000);
    for (int i = 0; i < clicks; ++i) {
      const char* v = mlt_properties_get(fp, "open_natron");
      mlt_properties_set(fp, "open_natron", v && !strcmp(v, "1") ? "0" : "1");
      usleep(300 * 1000);
    }
    usleep(500 * 1000); /* the request to the daemon is sent from a separate thread */
  }
  const char* xml = arg(argc, argv, "--save-xml", "");
  if (*xml) {
    mlt_consumer c = mlt_factory_consumer(profile, "xml", xml);
    if (!c) { fprintf(stderr, "cannot create xml consumer\n"); return 5; }
    mlt_consumer_connect(c, MLT_PRODUCER_SERVICE(filtered));
    mlt_consumer_start(c);
    mlt_consumer_stop(c);
    mlt_consumer_close(c);
  }
  mlt_properties_close(fake_consumer);
  mlt_filter_close(f);
  mlt_producer_close(filtered);
  mlt_producer_close(ref);
  mlt_profile_close(profile);
  mlt_factory_close();
  return other ? 1 : 0;
}
