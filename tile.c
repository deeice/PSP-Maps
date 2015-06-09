/* returns in buffer "b" the name of the Google Maps tile for location (x,y,z) */
char *GGtile(int x, int y, int z)
{
	static char b[99];
	int c = 18 - z;
	b[c] = '\0';
	while (z++ < 17)
	{
		c--;
		if (x % 2)
		{
			if (y % 2)
				b[c] = 's';
			else
				b[c] = 'r';
		}
		else
		{
			if (y % 2)
				b[c] = 't';
			else
				b[c] = 'q';
		}
		x/=2;
		y/=2;
	}
	b[0] = 't';
	return b;
}

/* returns in buffer "b" the name of the Virtual Earth tile for location (x,y,z) */
char *VEtile(int x, int y, int z)
{
	static char b[99];
	int c = 17 - z;
	b[c] = '\0';
	while (z++ < 17)
	{
		c--;
		if (x % 2)
		{
			if (y % 2)
				b[c] = '3';
			else
				b[c] = '1';
		}
		else
		{
			if (y % 2)
				b[c] = '2';
			else
				b[c] = '0';
		}
		x/=2;
		y/=2;
	}
	return b;
}

/* save tile in memory cache */
void savememory(int x, int y, int z, int s, SDL_Surface *tile)
{
	DEBUG("savememory(%d, %d, %d, %d)\n", x, y, z, s);
	SDL_FreeSurface(memory[memory_idx].tile);
	memory[memory_idx].x = x;
	memory[memory_idx].y = y;
	memory[memory_idx].z = z;
	memory[memory_idx].s = s;
	memory[memory_idx].tile = tile;
	memory_idx = (memory_idx + 1) % MEMORY_CACHE_SIZE;
}

/* return the disk file name for cache entry
 * maximum of 1000 entries per folder to improve access speed */
void diskname(char *buf, int n)
{
	/* create folders if needed */
	sprintf(buf, "cache/%.3d", n/1000);
	mkdir(buf, 0755);
	/* return the full file name */
	sprintf(buf, "cache/%.3d/%.3d.dat", n/1000, n%1000);
}

/* save tile in disk cache */
void savedisk(int x, int y, int z, int s, SDL_RWops *rw, int n)
{
	FILE *f;
	char name[50];
	char buffer[BUFFER_SIZE];
	
	if (!config.cache_size) return;
	
	DEBUG("savedisk(%d, %d, %d, %d)\n", x, y, z, s);
	
	if (rw == NULL)
	{
		printf("warning: savedisk(NULL)!\n");
		return;
	}
	
	disk[disk_idx].x = x;
	disk[disk_idx].y = y;
	disk[disk_idx].z = z;
	disk[disk_idx].s = s;
	
	SDL_RWseek(rw, 0, SEEK_SET);
	diskname(name, disk_idx);
	if ((f = fopen(name, "wb")) != NULL)
	{
		SDL_RWread(rw, buffer, 1, n);
		fwrite(buffer, 1, n, f);
		fclose(f);
	}
	
	disk_idx = (disk_idx + 1) % config.cache_size;
}

/* curl callback to save in memory */
size_t curl_write(void *ptr, size_t size, size_t nb, void *stream)
{
	SDL_RWops *rw = stream;
	int t = nb * size;
	rw->write(rw, ptr, size, nb);
	return t;
}

/* get the image on internet and return a buffer */
SDL_RWops *getnet(int x, int y, int z, int s)
{
	char request[1024];
	SDL_RWops *rw;
	
	DEBUG("getnet(%d, %d, %d, %d)\n", x, y, z, s);
	
	switch (s)
	{
		case GG_MAP:
			sprintf(request, _url[s], ++balancing%4, x, y, z);
			break;
		case GG_SATELLITE:
			sprintf(request, _url[s], ++balancing%4, x, y, z);
			break;
		case GG_HYBRID:
		case GG_TERRAIN:
			sprintf(request, _url[s], ++balancing%4, x, y, z);
			break;
		case VE_ROAD:
		case VE_AERIAL:
		case VE_HYBRID:
		case VE_HILL:
			sprintf(request, _url[s], VEtile(x, y, z));
			break;
		case YH_MAP:
		case YH_SATELLITE:
		case YH_HYBRID:
			sprintf(request, _url[s], x, (int) pow(2, 16-z)-y-1, z+1);
			break;
		case OS_MAPNIK:
                case OS_CLOUDMADE:
		case OS_OPENCYCLEMAP:
                case OS_OPENCYCLEMAPTRANS:
			sprintf(request, _url[s], 17-z, x, y);
			break;
                case MQ_MAPQUEST:
                case MQ_MAPQUESTAERIAL:
			sprintf(request, _url[s], 17-z, x, y);
			break;
		case GG_MOON_APOLLO:
		case GG_MOON_CLEMBW:
		case GG_MOON_ELEVATION:
			sprintf(request, _url[s], 17-z, x, (int) pow(2, 17-z)-y-1);
			break;
		case GG_MARS_ELEVATION:
		case GG_MARS_VISIBLE:
		case GG_MARS_INFRARED:
			sprintf(request, _url[s], GGtile(x, y, z));
			break;
		case GG_SKY_VISIBLE:
			sprintf(request, _url[s], x, y, 17-z);
			break;
		case GG_SKY_INFRARED:
		case GG_SKY_MICROWAVE:
		case GG_SKY_HISTORICAL:
			sprintf(request, _url[s], 17-z, x, y);
			break;
	}
        DEBUG("geturl('%s')\n", request);
	rw = SDL_RWFromMem(response, BUFFER_SIZE);
	
	//curl_easy_setopt(curl, CURLOPT_VERBOSE, 1);
	curl_easy_setopt(curl, CURLOPT_USERAGENT, "PSP-Maps " VERSION);
	curl_easy_setopt(curl, CURLOPT_URL, request);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write);
	curl_easy_setopt(curl, CURLOPT_WRITEDATA, rw);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10);
	
	if (curl_easy_perform(curl) != 0)
		/* if there was a network error, invalidate previous buffer */
		response[0] = '\0';
	
	return rw;
}

/* return the tile from disk if available, or NULL */
SDL_Surface *getdisk(int x, int y, int z, int s)
{
	int i;
	char name[50];
	DEBUG("getdisk(%d, %d, %d, %d)\n", x, y, z, s);
	for (i = 0; i < config.cache_size; i++)
		if (disk[i].x == x && disk[i].y == y && disk[i].z == z && disk[i].s == s)
		{
			diskname(name, i);
			return IMG_Load(name);
		}
	return NULL;
}

#if 1 /* ZIPIT_Z2 ENABLE_OFFLINE */
/* offline directory names for view types */
char *_offline[CHEAT_VIEWS] = {
	"Google/OSM_tiles",             // "Google Maps / Map",
	"Google/OSM_sat_tiles",         // "Google Maps / Satellite",
	"Google/OSM_hyb_tiles",         // "Google Maps / Hybrid",
	"Google/OSM_ter_tiles",         // "Google Maps / Terrain",
	"Virtual_Earth/OSM_tiles",      // "Virtual Earth / Road",
	"Virtual_Earth/OSM_sat_tiles",  // "Virtual Earth / Aerial",
	"Virtual_Earth/OSM_hyb_tiles",  // "Virtual Earth / Hybrid",
	"Virtual_Earth/OSM_ter_tiles",  // "Virtual Earth / Hill",
	"Yahoo/OSM_tiles",              // "Yahoo! Maps / Map",
	"Yahoo/OSM_sat_tiles",          // "Yahoo! Maps / Satellite",
	"Yahoo/OSM_hyb_tiles",          // "Yahoo! Maps / Hybrid",
	"OpenStreetMap/OSM_tiles",      // "OpenStreetMap / Mapnik",
	"CloudMade/OSM_tiles",          // "OpenStreetMap / CloudMade",
	"OpenCycleMap/OSM_tiles",       // "OpenCycleMap / Map",
	"OpenCycleMap/OSM_chart_tiles", // "OpenCycleMap / Transport",
	"MapQuest/OSM_tiles",           // "MapQuest / Map",
	"MapQuest/OSM_sat_tiles",       // "MapQuest / Open Aerial",
	"", // "",
	"Google_Moon/Apollo",
	"Google_Moon/Clem_BW",
	"Google_Moon/Elevation",
	"Google_Mars/Visible",
	"Google_Mars/Elevation",
	"Google_Mars/Infrared",
	"Google_Sky/Visible",
	"Google_Sky/Infrared",
	"Google_Sky/Microwave",
	"Google_Sky/Historical",
};

/* return the tile from OSM_tiles offline folder if available, or NULL */
SDL_Surface *getoffline(int x, int y, int z, int s)
{
	SDL_Surface *offline;
	char name[256];

	// Look for OSM_TILES formatted dir dumped by gmapcatcher.
	//printf("getoffline(%d, %d, %d, %d)\n", x, y, z, s);
	if ((_offline[s] == NULL) || !strlen(_offline[s]))
		return NULL;
	sprintf(name, "offline/%s/%d/%d/%d.png", _offline[s], 17-z, x, y);
	offline = IMG_Load(name);
	// if (offline)	printf("GOToffline(%d, %d, %d, %d)\n", x, y, z, s);
	return offline;
}
#endif

/* return the tile from memory if available, or NULL */
SDL_Surface *getmemory(int x, int y, int z, int s)
{
	int i;
	DEBUG("getmemory(%d, %d, %d, %d)\n", x, y, z, s);
	for (i = 0; i < MEMORY_CACHE_SIZE; i++)
		if (memory[i].tile && memory[i].x == x && memory[i].y == y && memory[i].z == z && memory[i].s == s)
			return memory[i].tile;
	return NULL;
}

/* downloads the image from Google for location (x,y,z) with mode (s) */
SDL_Surface* gettile(int x, int y, int z, int s)
{
	SDL_RWops *rw;
	SDL_Surface *tile;
	int n;
	
	/* try memory cache */
	if ((tile = getmemory(x, y, z, s)) != NULL)
		return tile;
	
#if 1 /* ZIPIT_Z2 ENABLE_OFFLINE */
	/* try disk offline (BEFORE disk cache, for now) */
	if ((tile = getoffline(x, y, z, s)) != NULL)
	{
		savememory(x, y, z, s, tile);
		return tile;
	}
#endif
	
	/* try disk cache */
	if ((tile = getdisk(x, y, z, s)) != NULL)
	{
		savememory(x, y, z, s, tile);
		return tile;
	}

        extern int netOff;
        
	if (netOff) {
            tile = zoomSurface(na, 1, 1, 0);
            savememory(x, y, z, s, tile);
            return tile;
        }

	/* try internet */
	rw = getnet(x, y, z, s);
	
	/* load the image */
	n = SDL_RWtell(rw);
	SDL_RWseek(rw, 0, SEEK_SET);
	tile = IMG_Load_RW(rw, 0);
	SDL_RWseek(rw, 0, SEEK_SET);
	
	/* if there is no tile, copy the n/a image
	 * I use a dummy call to zoomSurface to copy the surface
	 * because I had issues with SDL_DisplayFormat() on PSP */
	if (tile == NULL)
		tile = zoomSurface(na, 1, 1, 0);
	/* only save on disk if not n/a
	 * to avoid filling the cache with wrong images
	 * when we are offline */
	else
		savedisk(x, y, z, s, rw, n);
	savememory(x, y, z, s, tile);
	
	SDL_RWclose(rw);
	
	return tile;
}
