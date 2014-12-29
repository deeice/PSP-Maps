#include "global.h"
#include "kml.h"

#include <math.h>
#include <string.h>
#include <dirent.h>
#include <SDL_image.h>
#include <SDL_gfxPrimitives.h>

#ifdef GOOGLEMAPS_API2
#include <libxml/parser.h>
#include <libxml/tree.h>
#endif

SDL_Surface *marker;
Placemark *places = NULL;

#ifdef GOOGLEMAPS_API2
void placemark_parse(xmlNode *node, Placemark *place)
{
	xmlNode *cur, *cur2;
	
	for (cur = node->children; cur; cur = cur->next)
	{
		if (strcmp((char *) cur->name, "name") == 0)
			place->name = strdup((char *) cur->children->content);
		if (strcmp((char *) cur->name, "description") == 0)
			place->description = strdup((char *) cur->children->content);
		if (strcmp((char *) cur->name, "Point") == 0)
		{
			/* parse point */
			place->type = PLACEMARK_POINT;
			place->point = malloc(sizeof(PlacemarkPoint));
			place->marker = marker;
			/* scan for coordinates */
			for (cur2 = cur->children; cur2; cur2 = cur2->next)
			{
				if (strcmp((char *) cur2->name, "coordinates") == 0)
					sscanf((char *) cur2->children->content, "%f,%f,", &place->point->lon, &place->point->lat);
			}
		}
		if (strcmp((char *) cur->name, "GeometryCollection") == 0)
		{
			/* parse line */
			place->type = PLACEMARK_LINE;
			place->line = strdup("");
			/* scan for lines */
			for (cur2 = cur->children; cur2; cur2 = cur2->next)
				if (strcmp((char *) cur2->name, "LineString") == 0)
				{
					place->line = realloc(place->line, strlen(place->line) + strlen((char *) cur2->children->children->content) + 1);
					strcat(place->line, (char *) cur2->children->children->content);
				}
		}
	}
}

void kml_parse(char *file)
{
	xmlDoc *doc = NULL;
	xmlNode *node, *cur;

	doc = xmlReadFile(file, NULL, 0);
	if (doc == NULL)
	{
		DEBUG("KML error: no document!\n");
		return;
	}

	node = xmlDocGetRootElement(doc);
	if (node == NULL)
	{
		DEBUG("KML error: no root element!\n");
		return;
	}
	
	/* check the XML document starts with the KML node */
	if (strcmp((char *) node->name, "kml"))
	{
		DEBUG("KML error: no kml root element!\n");
		return;
	}
	node = node->children;
	
	/* skip Document and Folder nodes */
	while (node && (strcmp((char *) node->name, "Document") == 0 || strcmp((char *) node->name, "Folder") == 0))
		node = node->children;
	
	/* scan Placemarks */
	for (cur = node; cur; cur = cur->next)
		if (strcmp((char *) cur->name, "Placemark") == 0)
		{
			Placemark *tmp = malloc(sizeof(Placemark));
			placemark_parse(cur, tmp);
			tmp->next = places;
			places = tmp;
		}
	
	xmlFreeDoc(doc);
}

#else
#include "cJSON.h"

// Bounding box to zoom level (globe = 256 pixels at no zoom).
#define GLOBE_WIDTH 256
#define LN2 0.693147181f

int DecodePolylinePoints(char* polylinechars, Placemark * place, int zoom)
{
  char content[128];
  int i = 0;
  
  int currentLat = 0;
  int currentLng = 0;
  int next5bits;
  int sum;
  int shifter;
  int n;

  if (polylinechars == NULL || strlen(polylinechars) == 0) 
    return 0;
  n = strlen(polylinechars);
  while (i < n)
  {
    // calculate next latitude
    sum = 0;
    shifter = 0;
    do
    {
      next5bits = (int)polylinechars[i++] - 63;
      sum |= (next5bits & 31) << shifter;
      shifter += 5;
    } while (next5bits >= 32 && i < n);
    
    if (i >= n)
      break;
	  
    currentLat += (sum & 1) == 1 ? ~(sum >> 1) : (sum >> 1);
	  
    //calculate next longitude
    sum = 0;
    shifter = 0;
    do
    {
      next5bits = (int)polylinechars[i++] - 63;
      sum |= (next5bits & 31) << shifter;
      shifter += 5;
    } while (next5bits >= 32 && i < n);
	  
    if (i >= n && next5bits >= 32)
      break;
    
    currentLng += (sum & 1) == 1 ? ~(sum >> 1) : (sum >> 1);

    sprintf(content, "%f,%f,%f ", (double)(currentLng) / 100000.0,(double)(currentLat) / 100000.0, 0.0f); // zoom?

    place->line = realloc(place->line, strlen(place->line) + strlen(content) + 1);
    strcat(place->line, (char *) content);
  } 
  return 1;
}

void location_parse(cJSON *location, cJSON *address)
{
  Placemark *place = malloc(sizeof(Placemark));

  /* parse point */
  place->type = PLACEMARK_POINT;
  place->marker = marker;
  place->point = malloc(sizeof(PlacemarkPoint));
  place->point->lon = cJSON_GetObjectItem(location, "lng")->valuedouble;
  place->point->lat = cJSON_GetObjectItem(location, "lat")->valuedouble;
  if (address)
    place->name = strdup((char *) address->valuestring);
  place->next = places;
  places = place;
}

void bounds_parse(cJSON *bounds, int *zoom)
{
	if (bounds) {
		double east = 0.0;
		double west = 0.0;
		double angle;
		int z;
		cJSON * northeast = cJSON_GetObjectItem(bounds, "northeast");
		cJSON * southwest = cJSON_GetObjectItem(bounds, "southwest");
		if (northeast)
		    east = cJSON_GetObjectItem(northeast, "lng")->valuedouble;
		if (southwest)
		    west = cJSON_GetObjectItem(southwest, "lng")->valuedouble;
		// Convert bounding box to zoom level.
		angle = east - west;
		if (angle < 0.0f)
		    angle += 360.0f;
		z = 18 - round(log(WIDTH * 360.0f / angle / GLOBE_WIDTH) / LN2);
		if (z > 16) z = 16; // global
		if (z < 1) z = 1; // exact street address
		*zoom = z;
	}
}

void kml_parse(char *file)
{
	int i,j, zoom=0;
	cJSON *json = NULL;
	cJSON *status = NULL;
	cJSON *routes = NULL;
	cJSON *bounds = NULL;
	cJSON *legs = NULL;
	cJSON *overview_polyline = NULL;
	FILE *f;
	char *text;
	if (f=fopen(file,"rb")) {
		long len;
		fseek(f,0,SEEK_END); len=ftell(f); fseek(f,0,SEEK_SET);
		text=(char*)malloc(len+1); fread(text,1,len,f);
		fclose(f);
	}
	else return;
	json=cJSON_Parse(text);
	if (!json) return;
	status = cJSON_GetObjectItem(json, "status"); 
	routes = cJSON_GetObjectItem(json,"routes");
	if (status && (!strcmp(status->valuestring, "OK")) &&
	    routes && (cJSON_GetArraySize(routes) > 0)) {
		// Use the first (best?) route, ignore others.
		cJSON *route = cJSON_GetArrayItem(routes, 0);
		bounds = cJSON_GetObjectItem(route, "bounds");
		legs = cJSON_GetObjectItem(route, "legs");
		overview_polyline = cJSON_GetObjectItem(route, "overview_polyline");
	}
	if (bounds) 
	  bounds_parse(bounds, &zoom);
	// Should be one and only one leg because thats what we requested.
	if (legs) 
	  for (j = 0 ; j < cJSON_GetArraySize(legs) ; j++) {
		cJSON * leg = cJSON_GetArrayItem(legs, j);
		cJSON * start_location = cJSON_GetObjectItem(leg, "start_location");
		cJSON * start_address = cJSON_GetObjectItem(leg, "start_address");
		cJSON * end_location = cJSON_GetObjectItem(leg, "end_location");
		cJSON * end_address = cJSON_GetObjectItem(leg, "end_address");
		if (start_location, start_address)
		    location_parse(start_location, start_address);
		if (end_location, end_address)
		    location_parse(end_location, end_address);
	}
	if (overview_polyline) {
		char* points = cJSON_GetObjectItem(overview_polyline, "points")->valuestring;
		Placemark *place = malloc(sizeof(Placemark));
		place->type = PLACEMARK_LINE;
		place->line = strdup("");
		DecodePolylinePoints(points, place, zoom);
		place->next = places;
		places = place;
	}
	cJSON_Delete(json);
	free(text);
}

void kml_location(char *text, float *x, float *y, int *z)
{
	float lat = 0.0f, lon = 0.0f;
#ifdef USE_GOOGLE_XML
	int precision = 9; //subregion
	char *latstr, *lngstr;
	if ((latstr = strstr(text, "<lat>")) &&
	    (lngstr = strstr(text, "<lng>")) &&
	    (sscanf(latstr, "<lat>%f", &lat) == 1) &&
	    (sscanf(lngstr, "<lng>%f", &lon) == 1))
	{
		if (strstr(text, "<location_type>ROOFTOP"))
			precision = 1; //exact
		else if (strstr(text, "<location_type>RANGE_INTERPOLATED"))
			precision = 2; //intersection
		else if (strstr(text, "<location_type>GEOMETRIC_CENTER"))
			precision = 5; // postcode
		else if (strstr(text, "<location_type>APPROXIMATE"))
			precision = 7; // town
		DEBUG("precision: %d, lat: %f, lon: %f\n", precision, lat, lon);
		*z = precision;
		latlon2xy(lat, lon, x, y, *z);
	}
#else
	cJSON *json = NULL;
	cJSON *status = NULL;
	cJSON *results = NULL;
	cJSON *bounds = NULL;
	cJSON *location = NULL;
	//cJSON *address = NULL;
	json=cJSON_Parse(text);
	if (!json) return;
	status = cJSON_GetObjectItem(json, "status"); 
	results = cJSON_GetObjectItem(json,"results");
	if (status && (!strcmp(status->valuestring, "OK")) &&
	    results && (cJSON_GetArraySize(results) > 0)) {
		// Use the first (best?) result, ignore others.
		cJSON *result = cJSON_GetArrayItem(results, 0);
		cJSON *geometry = cJSON_GetObjectItem(result, "geometry");
		if (geometry) {
			bounds = cJSON_GetObjectItem(geometry, "bounds");
			if (!bounds) 
				bounds = cJSON_GetObjectItem(geometry, "viewport");
			location = cJSON_GetObjectItem(geometry, "location");
		}
		//address = cJSON_GetObjectItem(result, "formatted_address");
	}
	if (location)  {
		*z = 9; //subregion
		if (bounds) 
			bounds_parse(bounds, z);
		lon = cJSON_GetObjectItem(location, "lng")->valuedouble;
		lat = cJSON_GetObjectItem(location, "lat")->valuedouble;
		latlon2xy(lat, lon, x, y, *z);
	}
	// Put a pin a the location?
	//if (address) name = strdup((char *) address->valuestring);
	cJSON_Delete(json);
#endif
}
#endif

void kml_load()
{
	DIR *directory;
	struct dirent *entry;
	char file[100];
	SDL_Surface *default_marker;

#ifdef GOOGLEMAPS_API2
	LIBXML_TEST_VERSION
	xmlKeepBlanksDefault(0);
#endif	
	/* default marker */
	default_marker = IMG_Load("data/marker.png");
	
	/* parse the kml directory */
	if ((directory = opendir("kml/")) != NULL)
		while ((entry = readdir(directory)) != NULL)
			/* load only the .kml files */
			if (entry->d_name[0] != '.' && strcasecmp(strchr(entry->d_name, '.'), ".kml") == 0)
			{
				/* remove .kml suffix */
				strchr(entry->d_name, '.')[0] = '\0';
				/* try to load .png image */
				sprintf(file, "kml/%s.png", entry->d_name);
				DEBUG("kml_parse(\"%s\")\n", file);
				marker = IMG_Load(file);
				/* if not available, default marker */
				if (marker == NULL) marker = default_marker;
				/* load KML file */
				sprintf(file, "kml/%s.kml", entry->d_name);
				kml_parse(file);
			}
#ifdef GOOGLEMAPS_API2
	xmlCleanupParser();
#endif
}

void kml_free()
{
	Placemark *tmp = places;
	while (places)
	{
		tmp = places->next;
		free(places);
		places = tmp;
	}
}

void kml_display(SDL_Surface *dst, float x, float y, int z)
{
	Placemark *place = places;
	float nx, ny;
	while (place)
	{
		switch (place->type)
		{
			case PLACEMARK_POINT:
			{
				latlon2xy(place->point->lat, place->point->lon, &nx, &ny, z);
				SDL_Rect pos;
				pos.x = (nx-x)*256+WIDTH/2 - place->marker->w/2;
				pos.y = (ny-y)*256+HEIGHT/2 - place->marker->h/2;
				SDL_BlitSurface(place->marker, NULL, dst, &pos);
				break;
			}
			case PLACEMARK_LINE:
			{
				int init = 0;
				float ox = 0, oy = 0;
				char *copy = strdup(place->line);
				char *tmp;
				tmp = strtok(copy, " ");
				while (tmp)
				{
					float lat, lon;
					sscanf(tmp, "%f,%f,", &lon, &lat);
					latlon2xy(lat, lon, &nx, &ny, z);
					if (init)
						lineColor(dst, (ox-x)*256+WIDTH/2, (oy-y)*256+HEIGHT/2, (nx-x)*256+WIDTH/2, (ny-y)*256+HEIGHT/2, 0x0000ffaa);
					ox = nx;
					oy = ny;
					init = 1;
					tmp = strtok(NULL, " ");
				}
				free(copy);
				break;
			}
		}
		place = place->next;
	}
}
