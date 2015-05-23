#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <pthread.h>
#include <errno.h>

#include "gps.h"
#include "gpsdclient.h"

/******************************************************************/
enum unit (*GPSD_units)(void);
void (*GPSD_source_spec)(const char *fromstring,struct fixsource_t *source);

int (*GPS_open)(const char *, const char *,struct gps_data_t *);
int (*GPS_close)(struct gps_data_t *);
int (*GPS_read)(struct gps_data_t *);
int (*GPS_unpack)(char *, struct gps_data_t *);
bool (*GPS_waiting)(const struct gps_data_t *, int);
int (*GPS_stream)(struct gps_data_t *, unsigned int, void *);
const char *(*GPS_errstr)(const int);
void (*GPS_enable_debug)(int, FILE *);

/******************************************************************/
pthread_t gps_thread;
static pthread_mutex_t gps_mutex;

static struct gps_data_t gpsdata;
static float altfactor = METERS_TO_FEET;
static float speedfactor = MPS_TO_MPH;
static char *altunits = "ft";
static char *speedunits = "mph";
static struct fixsource_t source;
static int state = 0;		/* or MODE_NO_FIX=1, MODE_2D=2, MODE_3D=3 */
static float gps_latitude;
static float gps_longitude;
static float gps_altitude;

/******************************************************************/
int GPS_get_fix(float *latitude, float *longitude)
{ 
  int retval;
  // Lock the mutex
  pthread_mutex_lock(&gps_mutex);
  retval = state;
  // Copy the GPS data from protected zone to lat, long
  *latitude = gps_latitude;
  *longitude = gps_longitude;
  // Unlock the mutex
  pthread_mutex_unlock(&gps_mutex);
  
  return retval;
}

/******************************************************************/
int GPS_thread_quit(void)
{
  // Probably need to signal the gps thread to give it up before the join.

  /* We're done talking to gpsd. */
  (void)(*GPS_close)(&gpsdata);

  if(pthread_join(gps_thread, NULL))
  {
    printf("Could not join thread\n");
    return -1;
  }

  // Clean up the mutex
  pthread_mutex_destroy(&gps_mutex);

  return 0;

}

/******************************************************************/
void* GPS_thread_run(void *arg)
{
  unsigned int flags = WATCH_ENABLE;
  int wait_clicks = 0;  /* cycles to wait before gpsd timeout */

  switch ((*GPSD_units)()) {
  case imperial:
    altfactor = METERS_TO_FEET;
    altunits = "ft";
    speedfactor = MPS_TO_MPH;
    speedunits = "mph";
    break;
  case nautical:
    altfactor = METERS_TO_FEET;
    altunits = "ft";
    speedfactor = MPS_TO_KNOTS;
    speedunits = "knots";
    break;
  case metric:
    altfactor = 1;
    altunits = "m";
    speedfactor = MPS_TO_KPH;
    speedunits = "kph";
    break;
  default:
    /* leave the default alone */
    break;
  }

  (*GPSD_source_spec)(NULL, &source);

  /* Open the stream to gpsd. */
  if ((*GPS_open)(source.server, source.port, &gpsdata) != 0) {
    (void)fprintf(stderr,
		  "No gpsd running or network error: %d, %s\n",
		  errno, GPS_errstr(errno));
    pthread_exit(NULL); // die(errno == 0 ? GPS_GONE : GPS_ERROR);
  }

  if (source.device != NULL)
    flags |= WATCH_DEVICE;
  (void)(*GPS_stream)(&gpsdata, flags, source.device);
  
  for (;;) {
    int c;
    
    /* wait 1/2 second for gpsd */
    if (!(*GPS_waiting)(&gpsdata, 500000)) {
      /* 240 tries at .5 Sec a try is a 2 minute timeout */
      if ( 240 < wait_clicks++ ) 
	pthread_exit(NULL); // die(GPS_TIMEOUT);
    } 
    else {
      wait_clicks = 0;
      errno = 0;
      if ((*GPS_read)(&gpsdata) == -1) {
	fprintf(stderr, "Socket error 4\n");
	pthread_exit(NULL); // die(errno == 0 ? GPS_GONE : GPS_ERROR);
      } 
      /* Here's where updates go now that things are established. */
      else {
	pthread_mutex_lock(&gps_mutex);            // Lock the mutex
	// Copy the GPS fix data to protected zone
	state = gpsdata.fix.mode;	  
	if ((gpsdata.fix.latitude==gpsdata.fix.latitude) &&
	    (gpsdata.fix.longitude==gpsdata.fix.longitude)) {
	  gps_latitude = gpsdata.fix.latitude;   /* Fill in the latitude. */
	  gps_longitude = gpsdata.fix.longitude; /* Fill in the longitude. */
	}
	pthread_mutex_unlock(&gps_mutex);          // Unlock the mutex
      }
    }
  }
  return NULL;
}

/******************************************************************/
int GPS_thread_init(void)
{
    // Initialize the mutex
    if(pthread_mutex_init(&gps_mutex, NULL))
    {
      fprintf(stderr,"Unable to initialize a mutex\n");
      return -1;
    }

    if(pthread_create(&gps_thread, NULL, &GPS_thread_run, NULL))
    {
      fprintf(stderr,"Unable to spawn thread\n");
      return -1;
    }

    return 0;
}

/******************************************************************/
int GPS_load(void)
{
  void *handle;
  char *error;

  printf("hello\n");

  handle = dlopen("libgps.so.21", RTLD_LAZY);
  if (!handle) {
    fprintf(stderr, "%s\n", dlerror());
    return -1;
  }

  dlerror();    /* Clear any existing error */

  *(void **) (&GPSD_units) = dlsym(handle, "gpsd_units");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gpsd_units\n", error);
  *(void **) (&GPSD_source_spec) = dlsym(handle, "gpsd_source_spec");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gpsd_source_specr\n", error);

  *(void **) (&GPS_open) = dlsym(handle, "gps_open");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_open\n", error);
  *(void **) (&GPS_close) = dlsym(handle, "gps_close");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_close\n", error);
  *(void **) (&GPS_read) = dlsym(handle, "gps_read");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_read\n", error);
  *(void **) (&GPS_unpack) = dlsym(handle, "gps_unpack");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_unpack\n", error);
  *(void **) (&GPS_waiting) = dlsym(handle, "gps_waiting");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_waiting\n", error);
  *(void **) (&GPS_stream) = dlsym(handle, "gps_stream");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_stream\n", error);
  *(void **) (&GPS_errstr) = dlsym(handle, "gps_errstr");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_errstr\n", error);
  *(void **) (&GPS_enable_debug) = dlsym(handle, "gps_enable_debug");
  if ((error = dlerror()) != NULL) fprintf(stderr, "%s - gps_enable_debug\n", error);

  printf("Found gpsd DLL.\n");

  if (GPS_thread_init()) {
    printf("gpsd Thread failure.\n");
    return -2;
  }

  return 1;
}
