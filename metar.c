/*
 *
 * metar.c - a program for fetching METARs about a location
 * some rights reserved under the BSD License:

Copyright (c) 2013, Nijumi "Ninja" Ardetus-Libera
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the software nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

 *
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <memory.h>
#include <stdarg.h>
#include <time.h>
#include <errno.h>
#include <math.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include <libxml/xpath.h>

#define METARFLAG_DECODED  0x1 // decode the METAR before displaying it
#define METARFLAG_UPDATE   0x2 // force a retrieval from the internet
#define METARFLAG_NOTS     0x4 // ignore timestamps when retrieving from the cache
#define METARFLAG_COLOR    0x8 // attempt to output colors
#define METARFLAG_SPECIAL 0x10 // special formatting rules apply
#define METARFLAG_PURGE   0x20 // purge the entire cache before retrieving

#define METAR_MAXURL      8000
#define METAR_BUFSIZE      512
#define METAR_TINYBUFSIZE   64
#define METAR_BIGBUFSIZE  8192
#define METAR_REPLACEBUF    32

enum flight_rules
{
  METAR_CATEGORY_VFR = 0,
  METAR_CATEGORY_MVFR = 1,
  METAR_CATEGORY_IFR = 2,
  METAR_CATEGORY_LIFR = 3,
  METAR_CATEGORY_UNKNOWN = -1
};

enum metar_type_info
{
  METAR_TYPE_METAR = 0,
  METAR_TYPE_SPECI = 1,
  METAR_TYPE_UNKNOWN = -1
};

enum sky_cover_type
{
  METAR_SKYCOND_SKC = 0,
  METAR_SKYCOND_CLR = 1,
  METAR_SKYCOND_CAVOK = 2,
  METAR_SKYCOND_FEW = 3,
  METAR_SKYCOND_SCT = 4,
  METAR_SKYCOND_BKN = 5,
  METAR_SKYCOND_OVC = 6,
  METAR_SKYCOND_OVX = 7,
  METAR_SKYCOND_UNKNOWN = -1
};

#define METAR_QUALITY_CORRECTED     0x1
#define METAR_QUALITY_AUTO          0x2
#define METAR_QUALITY_AUTO_STATION  0x4
#define METAR_QUALITY_MAINTENANCE   0x8
#define METAR_QUALITY_NO_SIGNAL    0x10
#define METAR_QUALITY_NO_LIGHTNING 0x20
#define METAR_QUALITY_NO_FREEZING  0x40
#define METAR_QUALITY_NO_WEATHER   0x80

struct sky_condition_entry
{
  /*int*/ enum sky_cover_type sky_cover; // see METAR_SKYCOND_* above
  int cloud_base_ft_agl;
};

struct metar
{
  char raw_text[METAR_BUFSIZE];
  char station_id[5];
  time_t observation_time;
  float latitude;
  float longitude;
  float temp_c;
  float dewpoint_c;
  int wind_dir_degrees;
  int wind_speed_kt;
  int wind_gust_kt;
  float visibility_statute_mi;
  float altim_in_hg;
  float sea_level_pressure_mb;
  int quality_control_flags; // see METAR_QUALITY_* above
  char wx_string[METAR_TINYBUFSIZE];
  struct sky_condition_entry sky_condition[4];
  size_t sky_condition_count;
  /*int*/ enum flight_rules flight_category; // see METAR_CATEGORY_* above
  float three_hr_pressure_tendency_mb;
  float maxT_c;
  float minT_c;
  float maxT24hr_c;
  float minT24hr_c;
  float precip_in;
  float pcp3hr_in;
  float pcp6hr_in;
  float pcp24hr_in;
  float snow_in;
  int vert_vis_ft;
  /*int*/ enum metar_type_info metar_type; // see METAR_TYPE_* above
  float elevation_m;
};

struct document
{
  char *data;
  size_t len;
};

int isVfrWeather(enum sky_cover_type ceil);
const char *skyCondition(enum sky_cover_type ceil);
void cleanup(char *restrict url, char *restrict format, char *restrict path, struct document *restrict doc, CURL *restrict curl);
size_t writeDocument(void *data, size_t len, size_t width, void *rest);
xmlXPathObject *getXmlNodes(xmlDoc *restrict xml, const char *restrict xpath);
void init_metar(struct metar *weather);
int xmlToMetar(xmlDoc *restrict xml, struct metar *restrict weather, size_t count);
size_t xmlGetMetarCount(xmlDoc *xml);
void strReplace(const char *restrict needle, const char *restrict replacement, const char *restrict haystack, char *dest, size_t len);
void strReplaceTimeZulu(const char *restrict needle, time_t replacement, const char *restrict haystack, char *dest, size_t len);
void strReplaceTime(const char *restrict needle, time_t replacement, const char *restrict haystack, char *dest, size_t len);
void strReplaceInt(const char *restrict needle, int replacement, const char *restrict haystack, char *dest, size_t len);
void strReplaceFloat(const char *restrict needle, float replacement, const char *restrict haystack, char *dest, size_t len);
void strReplaceFloat2(const char *restrict needle, float replacement, const char *restrict haystack, char *dest, size_t len);
const char *flightConditions(enum flight_rules rules, int color);

int main(int argc, const char *argv[])
{
  int flags, // how should we retrieve the METARs?  (see above)
    i, j, k, // for loops, etc.
    hours,   // number of hours in the past to retrieve weather data
    entries, // max number of METARs to parse
    c;       // what's the current command line flag?

  char *format; // format string (for parts)
  size_t formatLen;

  char *url; // url to xml file
  size_t urlLen;

  char *path; // path to metar.xml and metar.cmd
  size_t pathLen;

  char cmdline[METAR_BUFSIZE];
  char cmdline_out[METAR_BUFSIZE];
  char tmp[METAR_BUFSIZE + 11];
  char buf[METAR_BUFSIZE];
  char ibuf[METAR_REPLACEBUF];

  char *buf1, *buf2;

  char request[METAR_MAXURL]; // final url to xml file with query string
  FILE *fp; // file handles to metar.xml and metar.cmd
  size_t fileLen;
  struct stat fs;
  
  //struct metar weather;
  struct metar *weatherReports;
  size_t reportCount;

  struct document doc;
  xmlDoc *xml;

  CURL *curl;
  CURLcode res;

  LIBXML_TEST_VERSION

  flags = 0;
  format = url = path = NULL;
  formatLen = urlLen = pathLen = 0;
  hours = 1;
  entries = 10;

  curl = NULL;
  xml = NULL;
  doc.data = malloc(1); // will be expanded by realloc()
  doc.len = 0;

  // retrieve command line args
  while ( (c = getopt(argc, (char * const *)argv, "de:f:h:np:tu:xG")) != -1 )
  {
    switch ( c )
    {
      case 'G':
      {
        // color output
        flags |= METARFLAG_COLOR;
        break;
      }
      case 'e':
      {
        // number of entries
        entries = atoi(optarg);
        break;
      }
      case 'd':
      {
        // decoded METAR
        flags |= METARFLAG_DECODED | METARFLAG_SPECIAL;
        break;
      }
      case 'f':
      {
        // formatted METAR
        flags |= METARFLAG_DECODED;
        formatLen = strlen(optarg);
        format = (char *)malloc(formatLen);
        if ( !format )
        {
          fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
          cleanup(url, format, path, &doc, curl);
          return 2;
        }
        strcpy(format, optarg);
        formatLen = strlen(format);
        break;
      }
      case 'h':
      {
        // number of hours to go back
        hours = atoi(optarg);
        break;
      }
      case 'n':
      {
        // force a redownload
        flags |= METARFLAG_UPDATE;
        break;
      }
      case 'p':
      {
        // store metar.xml and metar.cmd in specified path
        pathLen = strlen(optarg) + 11;
        path = (char *)malloc(pathLen);
        if ( !path )
        {
          fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
          cleanup(url, format, path, &doc, curl);
          return 2;
        }
        strcpy(path, optarg);
        if ( path[strlen(path) - 1] != '/' )
          strcat(path, "/");
        pathLen = strlen(path);
        break;
      }
      case 't':
      {
        // ignore timestamps
        flags |= METARFLAG_NOTS;
        break;
      }
      case 'u':
      {
        // change web service URL
        urlLen = strlen(optarg);
        url = (char *)malloc(urlLen);
        if ( !url )
        {
          fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
          cleanup(url, format, path, &doc, curl);
          return 2;
        }
        strcpy(url, optarg);
        break;
      }
      case 'x':
      {
        // purge cache
        flags |= METARFLAG_PURGE;
        break;
      }
      case '?':
      {
        // help info
        if ( (optopt == 'p') || (optopt == 'u') )
        {
          fprintf(stderr, "%s: error: Option -%c requires an argument.\n", argv[0], optopt);
        }
        else if ( optopt == '?' )
        {
          fputs("Usage: metar [-Gdefhnptux] WXS1 [WXS2 [...]]\n\tWXS1..n:\t4-digit ICAO weather station code\n\t-G\t\tenable color output\n\t-d\t\tdecode METAR text\n\t-e <num>\tdisplay no more than the specified number of entries\n\t-f <str>\toutputs the METAR using the specified format:\n\t\t\t{raw_text}\t\t\tthe raw METAR\n\t\t\t{station_id}\t\t\t4-digit ICAO weather station code\n\t\t\t{observation_time}\t\tthe Zulu time the METAR was observed\n\t\t\t{observation_time_local}\tthe local time the METAR was observed\n\t\t\t{latitude}\t\t\tthe decimal latitude of the station\n\t\t\t{longitude}\t\t\tthe decimal longitude of the station\n\t\t\t{temp_c}\t\t\tthe temperature in Celsius\n\t\t\t{temp_f}\t\t\tthe temperature in Fahrenheit\n\t\t\t{dewpoint_c}\t\t\tthe dewpoint temperature in Celsius\n\t\t\t{dewpoint_f}\t\t\tthe dewpoint temperature in Fahrenheit\n\t\t\t{wind_dir_degrees}\t\tdirection from which the wind is coming, or 0 for variable\n\t\t\t{wind_speed_kt}\t\t\twind speed in knots\n\t\t\t{wind_gust_kt}\t\t\twind gust speed in knots\n\t\t\t{visibility_statute_mi}\t\thorizontal visibility in miles\n\t\t\t{altim_in_hg}\t\t\tstation pressure in inches of mercury\n\t\t\t{sea_level_pressure_mb}\t\tsea-level pressure in millibars\n\t\t\t{quality_control_flags}\t\tremarks about the station\n\t\t\t{wx_string}\t\t\tadverse weather information\n\t\t\t{sky_conditions}\t\tcloud cover and vertical visibility information\n\t\t\t{flight_category}\t\tVFR, MVFR, IFR, or LIFR\n\t\t\t{precip_in}\t\t\tprecipitation in inches\n\t\t\t{snow_in}\t\t\tsnow in inches\n\t\t\t{vert_vis_ft}\t\t\tvertical visibility in feet\n\t\t\t{elevation_m}\t\t\tstation elevation in meters\n\t-h <num>\tthe number of hours in the past to track\n\t-n\t\tforce a redownload of the METAR\n\t-p <path>\tchange cache path (default /tmp/ => /tmp/metar-*.xml)\n\t-t\t\tdon't download a METAR if one is available from the cache\n\t-u <url>\tchange the base URL of the METAR service\n\t-x\t\tpurge the cache before retrieval\n", stderr);
          cleanup(url, format, path, &doc, curl);
          return 0;
        }
        else if ( isprint(optopt) )
        {
          fprintf(stderr, "%s: error: Unknown option `-%c'.\n", argv[0], optopt);
        }
        else
        {
          fprintf(stderr, "%s: error: Unknown option character `\\x%x'.\n", argv[0], optopt);
        }
        cleanup(url, format, path, &doc, curl);
        return 1;
      }
    }
  }

  if ( url == NULL )
  {
    url = strdup("http://aviationweather.gov/adds/dataserver_current/httpparam");
    if ( !url )
    {
      fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
      cleanup(url, format, path, &doc, curl);
      return 2;
    }

    urlLen = strlen(url);
  }

  if ( format == NULL )
  {
    format = strdup("{raw_text}\n"); // TODO: change
    if ( !format )
    {
      fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
      cleanup(url, format, path, &doc, curl);
      return 2;
    }

    formatLen = strlen(format);
  }

  if ( path == NULL )
  {
    path = strdup("/tmp/");
    if ( !path )
    {
      fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
      cleanup(url, format, path, &doc, curl);
      return 2;
    }

    pathLen = strlen(path);
  }

  if ( (flags & METARFLAG_PURGE) == METARFLAG_PURGE )
  {
    sprintf(buf, "rm -f %smetar-*.xml", path); // TODO: make this secure
    system(buf);
  }

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();
  if ( !curl )
  {
    fprintf(stderr, "%s: error: Cannot initialize CURL.\n", argv[0]);
    cleanup(url, format, path, &doc, curl);
    return 3;
  }

  if ( optind >= argc )
  {
    if ( (flags & METARFLAG_PURGE) == METARFLAG_PURGE )
    {
      fprintf(stderr, "%s: Cache purged.\n", argv[0]);
      cleanup(url, format, path, &doc, curl);
      return 0;
    }
    else
    {
      fprintf(stderr, "%s: error: Please specify a weather station by 4-digit ICAO code.\n", argv[0]);
      cleanup(url, format, path, &doc, curl);
    }
    return 4;
  }

  for ( i = optind; i < argc; ++i )
  {
    // first, check if we're cached.
    strncpy(tmp, path, METAR_BUFSIZE);
    strcat(tmp, "metar-");
    strncat(tmp, argv[i], METAR_BUFSIZE - strlen(path));
    strcat(tmp, ".xml");

    doc.data[0] = '\0';
    doc.len = 0;

    memset((void *)&fs, 0, sizeof(struct stat));
    if ( (lstat(tmp, &fs) == 0) && ((flags & METARFLAG_UPDATE) != METARFLAG_UPDATE) )
    {
      if ( ((time(NULL) - fs.st_mtime) < 900) || ((flags & METARFLAG_NOTS) == METARFLAG_NOTS) )
      {
        fp = fopen(tmp, "r");
        if ( fp )
        {
          fseek(fp, 0, SEEK_END);
          fileLen = ftell(fp);
          fseek(fp, 0, SEEK_SET);
          doc.data = realloc(doc.data, fileLen + 1);
          if ( doc.data == NULL )
          {
            fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
            cleanup(url, format, path, &doc, curl);
            return 2;
          }
          doc.len = fileLen;
          fread(doc.data, 1, doc.len, fp);
          fclose(fp);
        }
      }
    }

    if ( doc.len == 0 )
    {
      snprintf(request, METAR_MAXURL,
        "%s?dataSource=metars&requestType=retrieve&format=xml&stationString=%s&hoursBeforeNow=%d",
        url,
        argv[i],
        hours);
      request[METAR_MAXURL - 1] = '\0';
      curl_easy_setopt(curl, CURLOPT_URL, request);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeDocument);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&doc);
      curl_easy_setopt(curl, CURLOPT_USERAGENT, "Metar/1.0");

      res = curl_easy_perform(curl);
      if ( res != CURLE_OK )
      {
        printf("No weather information for %s: %s.\n", argv[i], curl_easy_strerror(res));
        doc.len = 0;
      }
      else
      {
        unlink(tmp);
        fp = fopen(tmp, "w");
        if ( fp )
        {
          fwrite(doc.data, 1, doc.len, fp);
          fclose(fp);
        }
      }
    }

    if ( doc.len != 0 )
    {
      // we have our data, presumably.  fire up XML!
      xml = xmlReadMemory(doc.data, doc.len, "metar.xml", NULL, 0);

      reportCount = xmlGetMetarCount(xml);
      if ( reportCount > 0 )
      {
        weatherReports = (struct metar *)malloc(sizeof(struct metar) * reportCount);
        if ( weatherReports == NULL )
        {
          fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
          cleanup(url, format, path, &doc, curl);
          return 2;
        }
        switch ( xmlToMetar(xml, weatherReports, reportCount) )
        {
          case -3:
            printf("No weather information for %s: invalid XPath expression.\n", argv[i]);
            goto startOver;
          case -2:
            printf("No weather information for %s: invalid XPath context.\n", argv[i]);
            goto startOver;
          case -1:
            printf("No weather information for %s: invalid XML data.\n", argv[i]);
            goto startOver;
          case 0:
            printf("No weather information for %s is available at this time.\n", argv[i]);
            goto startOver;
        }

        for ( j = 0; (j < reportCount) && (j < entries); ++j )
        {
          if ( (flags & METARFLAG_DECODED) != METARFLAG_DECODED )
          {
            // output raw, I guess.
            printf("%s\n", weatherReports[j].raw_text);
          }
          else if ( (flags & METARFLAG_SPECIAL) == METARFLAG_SPECIAL )
          {
            // build our format.
            buf1 = (char *)malloc(METAR_BIGBUFSIZE);
            buf2 = (char *)malloc(METAR_BUFSIZE);
            if ( !buf1 || !buf2 )
            {
              fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
              xmlFreeDoc(xml);
              cleanup(url, format, path, &doc, curl);
              return 2;
            }

            strftime(buf2, METAR_BUFSIZE, "%Y-%m-%d %H:%M:%S",
              gmtime(&weatherReports[j].observation_time));

            sprintf(buf1,
              "%s (%.2f, %.2f) [%s] at %s\n",
              weatherReports[j].station_id,
              weatherReports[j].latitude,
              weatherReports[j].longitude,
              flightConditions(weatherReports[j].flight_category,
                (flags & METARFLAG_COLOR) == METARFLAG_COLOR ? 1 : 0),
              buf2);

            strftime(buf2, METAR_BUFSIZE, "%Y-%m-%d %H:%M:%S",
              localtime(&weatherReports[j].observation_time));

            snprintf(buf, METAR_BUFSIZE,
              "(Local time: %s)\n",
              buf2);
            buf[METAR_BUFSIZE - 1] = '\0';

            strcat(buf1, buf);

            // corrected?
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_CORRECTED) == METAR_QUALITY_CORRECTED )
            {
              if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                strcat(buf1, "\033[1;33mCorrected version\033[0m\n");
              else
                strcat(buf1, "Corrected version\n");
            }

            strcat(buf1, "\n");

            // winds
            if ( weatherReports[j].wind_dir_degrees >= 0 )
            {
              if ( weatherReports[j].wind_speed_kt == 0 )
              {
                strcat(buf1, "\tWinds: Calm\n");
              }
              else
              {
                if ( weatherReports[j].wind_dir_degrees == 0 )
                {
                  if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  {
                    if ( weatherReports[j].wind_speed_kt >= 10 )
                      snprintf(buf, METAR_BUFSIZE, "\033[1;31m%d knots\033[0m", weatherReports[j].wind_speed_kt);
                    else
                      snprintf(buf, METAR_BUFSIZE, "%d knots", weatherReports[j].wind_speed_kt);
                    buf[METAR_BUFSIZE - 1] = '\0';

                    strcat(buf1, "\tWinds: Variable at ");
                    strcat(buf1, buf);

                    if ( weatherReports[j].wind_gust_kt > 0 )
                    {
                      if ( (weatherReports[j].wind_gust_kt - weatherReports[j].wind_speed_kt) >= 5 )
                        snprintf(buf, METAR_BUFSIZE, " \033[1;31mgusting %d knots\033[0m", weatherReports[j].wind_gust_kt);
                      else
                        snprintf(buf, METAR_BUFSIZE, " gusting %d knots", weatherReports[j].wind_gust_kt);
                      buf[METAR_BUFSIZE - 1] = '\0';
                      strcat(buf1, buf);
                    }

                    strcat(buf1, "\n");
                  }
                  else
                  {
                    if ( weatherReports[j].wind_gust_kt > 0 )
                      snprintf(buf, METAR_BUFSIZE,
                        "\tWinds: Variable at %d knots gusting %d knots\n",
                        weatherReports[j].wind_speed_kt,
                        weatherReports[j].wind_gust_kt);
                    else
                      snprintf(buf, METAR_BUFSIZE,
                        "\tWinds: Variable at %d knots\n",
                        weatherReports[j].wind_speed_kt);
                    buf[METAR_BUFSIZE - 1] = '\0';
                    strcat(buf1, buf);
                  }
                }
                else
                {
                  if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  {
                    if ( weatherReports[j].wind_speed_kt >= 10 )
                      snprintf(buf, METAR_BUFSIZE, "%d* at \033[1;31m%d knots\033[0m",
                        weatherReports[j].wind_dir_degrees,
                        weatherReports[j].wind_speed_kt);
                    else
                      snprintf(buf, METAR_BUFSIZE, "%d* at %d knots",
                        weatherReports[j].wind_dir_degrees,
                        weatherReports[j].wind_speed_kt);
                    buf[METAR_BUFSIZE - 1] = '\0';

                    strcat(buf1, "\tWinds: ");
                    strcat(buf1, buf);

                    if ( weatherReports[j].wind_gust_kt > 0 )
                    {
                      if ( (weatherReports[j].wind_gust_kt - weatherReports[j].wind_speed_kt) >= 5 )
                        snprintf(buf, METAR_BUFSIZE, " \033[1;31mgusting %d knots\033[0m", weatherReports[j].wind_gust_kt);
                      else
                        snprintf(buf, METAR_BUFSIZE, " gusting %d knots", weatherReports[j].wind_gust_kt);
                      buf[METAR_BUFSIZE - 1] = '\0';
                      strcat(buf1, buf);
                    }

                    strcat(buf1, "\n");
                  }
                  else
                  {
                    if ( weatherReports[j].wind_gust_kt > 0 )
                      snprintf(buf, METAR_BUFSIZE,
                        "\tWinds: %d* at %d knots gusting %d knots\n",
                        weatherReports[j].wind_dir_degrees,
                        weatherReports[j].wind_speed_kt,
                        weatherReports[j].wind_gust_kt);
                    else
                      snprintf(buf, METAR_BUFSIZE,
                        "\tWinds: %d* at %d knots\n",
                        weatherReports[j].wind_dir_degrees,
                        weatherReports[j].wind_speed_kt);
                    buf[METAR_BUFSIZE - 1] = '\0';
                    strcat(buf1, buf);
                  }
                }
              }
            }

            // visibility
            if ( !isnan(weatherReports[j].visibility_statute_mi) )
            {
              if ( ((flags & METARFLAG_COLOR) == METARFLAG_COLOR) && (weatherReports[j].visibility_statute_mi < 5.0f) )
              {
                if ( weatherReports[j].visibility_statute_mi >= 3.0f )
                  snprintf(buf, METAR_BUFSIZE,
                    "\tVisibility: \033[1;34m%.1f miles\033[0m\n",
                    weatherReports[j].visibility_statute_mi);
                else if ( weatherReports[j].visibility_statute_mi >= 1.0f )
                  snprintf(buf, METAR_BUFSIZE,
                    "\tVisibility: \033[1;31m%.1f miles\033[0m\n",
                    weatherReports[j].visibility_statute_mi);
                else
                  snprintf(buf, METAR_BUFSIZE,
                    "\tVisibility: \033[1;35m%.1f miles\033[0m\n",
                    weatherReports[j].visibility_statute_mi);
              }
              else
              {
                snprintf(buf, METAR_BUFSIZE,
                  "\tVisibility: %.1f miles\n",
                  weatherReports[j].visibility_statute_mi);
              }
              buf[METAR_BUFSIZE - 1] = '\0';
              strcat(buf1, buf);
            }

            // sky conditions
            if ( weatherReports[j].sky_condition_count > 0 )
            {
              for ( k = 0; k < weatherReports[j].sky_condition_count; ++k )
              {
                if ( weatherReports[j].sky_condition[k].sky_cover == METAR_SKYCOND_CLR )
                {
                  strcat(buf1, "\tSky condition: Clear\n");
                }
                else
                {
                  if ( ((flags & METARFLAG_COLOR) == METARFLAG_COLOR) && (!isVfrWeather(weatherReports[j].sky_condition[k].sky_cover)) && (weatherReports[j].sky_condition[k].cloud_base_ft_agl <= 3000) )
                  {
                    if ( weatherReports[j].sky_condition[k].cloud_base_ft_agl >= 1000 )
                      snprintf(buf, METAR_BUFSIZE,
                        "\tSky condition: \033[1;34m%s at %d feet\033[0m above ground level\n",
                        skyCondition(weatherReports[j].sky_condition[k].sky_cover),
                        weatherReports[j].sky_condition[k].cloud_base_ft_agl);
                    else if ( weatherReports[j].sky_condition[k].cloud_base_ft_agl >= 500 )
                      snprintf(buf, METAR_BUFSIZE,
                        "\tSky condition: \033[1;31m%s at %d feet\033[0m above ground level\n",
                        skyCondition(weatherReports[j].sky_condition[k].sky_cover),
                        weatherReports[j].sky_condition[k].cloud_base_ft_agl);
                    else
                      snprintf(buf, METAR_BUFSIZE,
                        "\tSky condition: \033[1;35m%s at %d feet\033[0m above ground level\n",
                        skyCondition(weatherReports[j].sky_condition[k].sky_cover),
                        weatherReports[j].sky_condition[k].cloud_base_ft_agl);
                  }
                  else
                  {
                    snprintf(buf, METAR_BUFSIZE,
                      "\tSky condition: %s at %d feet above ground level\n",
                      skyCondition(weatherReports[j].sky_condition[k].sky_cover),
                      weatherReports[j].sky_condition[k].cloud_base_ft_agl);
                  }
                  buf[METAR_BUFSIZE - 1] = '\0';
                  strcat(buf1, buf);
                }
              }
            }

            // temperature
            if ( !isnan(weatherReports[j].temp_c) )
            {
              snprintf(buf, METAR_BUFSIZE, "\tTemperature: %.1f*C (%.1f*F)\n",
                weatherReports[j].temp_c,
                weatherReports[j].temp_c * 9.0f/5.0f + 32.0f);
              buf[METAR_BUFSIZE - 1] = '\0';
              strcat(buf1, buf);
            }

            // dewpoint
            if ( !isnan(weatherReports[j].dewpoint_c) )
            {
              snprintf(buf, METAR_BUFSIZE, "\tDewpoint: %.1f*C (%.1f*F)\n",
                weatherReports[j].dewpoint_c,
                weatherReports[j].dewpoint_c * 9.0f/5.0f + 32.0f);
              buf[METAR_BUFSIZE - 1] = '\0';
              strcat(buf1, buf);
            }

            // altimeter
            if ( !isnan(weatherReports[j].altim_in_hg) )
            {
              snprintf(buf, METAR_BUFSIZE, "\tPressure: %.2f\" Hg (%.1f mb)\n",
                weatherReports[j].altim_in_hg,
                33.85f * weatherReports[j].altim_in_hg);
              buf[METAR_BUFSIZE - 1] = '\0';
              strcat(buf1, buf);
            }

            // adverse weather
            if ( weatherReports[j].wx_string[0] != '\0' )
            {
              strcat(buf1, "\tAdverse weather: ");
              if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                strcat(buf1, "\033[1;33m");
              strcat(buf1, weatherReports[j].wx_string);
              if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                strcat(buf1, "\033[0m");
              strcat(buf1, "\n");
            }

            // notes
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_MAINTENANCE) == METAR_QUALITY_MAINTENANCE )
            {
              if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                strcat(buf1, "\t\033[1;33mWarning\033[0m: Station needs maintenance\n");
              else
                strcat(buf1, "\tWarning: Station needs maintenance\n");
            }

            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_NO_WEATHER) == METAR_QUALITY_MAINTENANCE )
            {
              if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                strcat(buf1, "\t\033[1;31mWarning\033[0m: Station offline\n");
              else
                strcat(buf1, "\tWarning: Station offline\n");
            }

            if ( (weatherReports[j].quality_control_flags & (METAR_QUALITY_AUTO | METAR_QUALITY_AUTO_STATION)) )
            {
              strcat(buf1, "\tAutomated weather available.\n");
            }

            // raw
            strcat(buf1, "\t");
            strcat(buf1, weatherReports[j].raw_text);
            strcat(buf1, "\n");

            puts(buf1);
            free(buf1);
            free(buf2);
          }
          else
          {
            // formatting time!
            buf1 = (char *)malloc(METAR_BIGBUFSIZE);
            buf2 = (char *)malloc(METAR_BIGBUFSIZE);

            if ( !buf1 || !buf2 )
            {
              fprintf(stderr, "%s: error: Out of memory.\n", argv[0]);
              xmlFreeDoc(xml);
              cleanup(url, format, path, &doc, curl);
              return 2;
            }

            strReplace("{raw_text}", weatherReports[j].raw_text, format, buf1, METAR_BIGBUFSIZE);
            strReplace("{station_id}", weatherReports[j].station_id, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceTimeZulu("{observation_time}", weatherReports[j].observation_time, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceTime("{observation_localtime}", weatherReports[j].observation_time, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat2("{latitude}", weatherReports[j].latitude, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat2("{longitude}", weatherReports[j].longitude, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{temp_c}", weatherReports[j].temp_c, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{dewpoint_c}", weatherReports[j].dewpoint_c, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{temp_f}", weatherReports[j].temp_c * 9.0f/5.0f + 32.0f, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{dewpoint_f}", weatherReports[j].dewpoint_c * 9.0f/5.0f + 32.0f, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceInt("{wind_dir_degrees}", weatherReports[j].wind_dir_degrees, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceInt("{wind_speed_kt}", weatherReports[j].wind_speed_kt, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceInt("{wind_gust_kt}", weatherReports[j].wind_gust_kt, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{visibility_statute_mi}", weatherReports[j].visibility_statute_mi, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat2("{altim_in_hg}", weatherReports[j].altim_in_hg, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat2("{sea_level_pressure_mb}", weatherReports[j].altim_in_hg, buf1, buf2, METAR_BIGBUFSIZE);
            strReplace("{wx_string}", weatherReports[j].wx_string, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat2("{three_hr_pressure_tendency_mb}", weatherReports[j].three_hr_pressure_tendency_mb, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{maxT_c}", weatherReports[j].maxT_c, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{minT_c}", weatherReports[j].minT_c, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{maxT24hr_c}", weatherReports[j].maxT24hr_c, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{minT24hr_c}", weatherReports[j].minT24hr_c, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{precip_in}", weatherReports[j].precip_in, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{pcp3hr_in}", weatherReports[j].pcp3hr_in, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{pcp6hr_in}", weatherReports[j].pcp6hr_in, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceFloat("{pcp24hr_in}", weatherReports[j].pcp24hr_in, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{snow_in}", weatherReports[j].snow_in, buf2, buf1, METAR_BIGBUFSIZE);
            strReplaceInt("{vert_vis_ft}", weatherReports[j].vert_vis_ft, buf1, buf2, METAR_BIGBUFSIZE);
            strReplaceFloat("{elevation_m}", weatherReports[j].elevation_m, buf2, buf1, METAR_BIGBUFSIZE);

            // quality control flags
            strcpy(buf, "");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_CORRECTED) == METAR_QUALITY_CORRECTED )
              strcat(buf, "COR ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_AUTO) == METAR_QUALITY_AUTO )
              strcat(buf, "AUTO ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_AUTO_STATION) == METAR_QUALITY_AUTO_STATION )
              strcat(buf, "AUTOST ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_MAINTENANCE) == METAR_QUALITY_MAINTENANCE )
              strcat(buf, "MAINT ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_NO_SIGNAL) == METAR_QUALITY_NO_SIGNAL )
              strcat(buf, "NOSIG ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_NO_LIGHTNING) == METAR_QUALITY_NO_LIGHTNING )
              strcat(buf, "NOLTN ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_NO_FREEZING) == METAR_QUALITY_NO_FREEZING )
              strcat(buf, "NOFRZ ");
            if ( (weatherReports[j].quality_control_flags & METAR_QUALITY_NO_WEATHER) == METAR_QUALITY_NO_WEATHER )
              strcat(buf, "INOP ");
            if ( buf[0] != '\0' )
              buf[strlen(buf) - 1] = '\0';
            strReplace("{quality_control_flags}", buf, buf1, buf2, METAR_BIGBUFSIZE);

            // sky condition
            strcpy(buf, "");
            for ( k = 0; k < weatherReports[j].sky_condition_count; ++k )
            {
              switch ( weatherReports[j].sky_condition[k].sky_cover )
              {
                case METAR_SKYCOND_SKC:
                  strcat(buf, "SKC");
                  break;
                case METAR_SKYCOND_CLR:
                  strcat(buf, "CLR");
                  break;
                case METAR_SKYCOND_CAVOK:
                  strcat(buf, "CAVOK");
                  break;
                case METAR_SKYCOND_FEW:
                  strcat(buf, "FEW");
                  break;
                case METAR_SKYCOND_SCT:
                  strcat(buf, "SCT");
                  break;
                case METAR_SKYCOND_BKN:
                  strcat(buf, "BKN");
                  break;
                case METAR_SKYCOND_OVC:
                  strcat(buf, "OVC");
                  break;
                case METAR_SKYCOND_OVX:
                  strcat(buf, "OVX");
                  break;
                default:
                  strcat(buf, "???");
                  break;
              }

              if ( weatherReports[j].sky_condition[k].sky_cover != METAR_SKYCOND_CLR )
              {
                sprintf(ibuf, "%d", weatherReports[j].sky_condition[k].cloud_base_ft_agl);
                strcat(buf, ibuf);
              }

              strcat(buf, " ");
            }
            if ( buf[0] != '\0' )
              buf[strlen(buf) - 1] = '\0';
            strReplace("{sky_condition}", buf, buf2, buf1, METAR_BIGBUFSIZE);

            if ( weatherReports[j].metar_type == METAR_TYPE_SPECI )
              strReplace("{metar_type}", "SPECI", buf1, buf2, METAR_BIGBUFSIZE);
            else
              strReplace("{metar_type}", "METAR", buf1, buf2, METAR_BIGBUFSIZE);

            strReplace("{flight_category}",
              flightConditions(weatherReports[j].flight_category,
                (flags & METARFLAG_COLOR) == METARFLAG_COLOR ? 1 : 0),
              buf2,
              buf1,
              METAR_BIGBUFSIZE);

            /*switch ( weatherReports[j].flight_category )
            {
              case METAR_CATEGORY_VFR:
                if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  strReplace("{flight_category}", "\033[1;32mVFR\033[0m", buf2, buf1, METAR_BIGBUFSIZE);
                else
                  strReplace("{flight_category}", "VFR", buf2, buf1, METAR_BIGBUFSIZE);
                break;
              case METAR_CATEGORY_MVFR:
                if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  strReplace("{flight_category}", "\033[1;34mMVFR\033[0m", buf2, buf1, METAR_BIGBUFSIZE);
                else
                  strReplace("{flight_category}", "MVFR", buf2, buf1, METAR_BIGBUFSIZE);
                break;
              case METAR_CATEGORY_IFR:
                if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  strReplace("{flight_category}", "\033[1;31mIFR\033[0m", buf2, buf1, METAR_BIGBUFSIZE);
                else
                  strReplace("{flight_category}", "IFR", buf2, buf1, METAR_BIGBUFSIZE);
                break;
              case METAR_CATEGORY_LIFR:
                if ( (flags & METARFLAG_COLOR) == METARFLAG_COLOR )
                  strReplace("{flight_category}", "\033[1;35mLIFR\033[0m", buf2, buf1, METAR_BIGBUFSIZE);
                else
                  strReplace("{flight_category}", "LIFR", buf2, buf1, METAR_BIGBUFSIZE);
                break;
              default:
                strReplace("{flight_category}", "???", buf2, buf1, METAR_BIGBUFSIZE);
                break;
            }*/

            // finally, output the buffer to the screen.
            puts(buf1);

            free(buf2);
            free(buf1);
          }
        }
      }

      startOver:
      xmlFreeDoc(xml);

#ifndef METAR_NO_THROTTLE
      if ( (i + 1) < argc )
        sleep(1); // to prevent server throttling
#endif
    }
  }

  cleanup(url, format, path, &doc, curl);
  return 0;
}

const char *flightConditions(enum flight_rules rules, int color)
{
  switch ( rules )
  {
    case METAR_CATEGORY_VFR:
      if ( color )
        return "\033[1;32mVFR\033[0m";
      else
        return "VFR";
    case METAR_CATEGORY_MVFR:
      if ( color )
        return "\033[1;34mMVFR\033[0m";
      else
        return "MVFR";
    case METAR_CATEGORY_IFR:
      if ( color )
        return "\033[1;31mIFR\033[0m";
      else
        return "IFR";
    case METAR_CATEGORY_LIFR:
      if ( color )
        return "\033[1;35mLIFR\033[0m";
      else
        return "LIFR";
    default:
      return "???";
  }
}

void cleanup(char *restrict url, char *restrict format, char *restrict path, struct document *restrict doc, CURL *restrict curl)
{
  if ( url ) free((void *)url);
  if ( format ) free((void *)format);
  if ( path ) free((void *)path);

  if ( doc )
  {
    if ( doc->data ) free(doc->data);
    doc->data = NULL;
    doc->len = 0;
  }

  if ( curl )
  {
    curl_easy_cleanup(curl);
    curl_global_cleanup();
  }

  xmlCleanupParser();
}

size_t writeDocument(void *data, size_t len, size_t width, void *rest)
{
  size_t actual = len * width;
  struct document *mem = (struct document *)rest;

  mem->data = realloc(mem->data, mem->len + actual + 1);
  if ( !mem->data )
  {
    fputs("Not enough memory to write the XML document.\n", stderr);
    return 0;
  }

  memcpy(&(mem->data[mem->len]), data, actual);
  mem->len += actual;
  mem->data[mem->len] = 0;

  return actual;
}

void init_metar(struct metar *weather)
{
  //memset((void *)weather, 0, sizeof(struct metar));
  weather->flight_category = METAR_CATEGORY_UNKNOWN;
  weather->metar_type = METAR_TYPE_UNKNOWN;
  weather->observation_time = 0;
  weather->quality_control_flags = 0;
  weather->raw_text[0] =
    weather->station_id[0] =
    weather->wx_string[0] = '\0';
  weather->vert_vis_ft =
    weather->wind_dir_degrees =
    weather->wind_gust_kt =
    weather->wind_speed_kt = -1;
  weather->altim_in_hg =
    weather->dewpoint_c =
    weather->elevation_m =
    weather->latitude =
    weather->longitude =
    weather->maxT24hr_c =
    weather->maxT_c =
    weather->minT24hr_c =
    weather->minT_c =
    weather->pcp24hr_in =
    weather->pcp3hr_in =
    weather->pcp6hr_in =
    weather->precip_in =
    weather->sea_level_pressure_mb =
    weather->snow_in =
    weather->temp_c =
    weather->three_hr_pressure_tendency_mb =
    weather->visibility_statute_mi =
    0.0f / 0.0f; // NaN
  weather->sky_condition_count = 0;
  weather->sky_condition[0].cloud_base_ft_agl =
    weather->sky_condition[1].cloud_base_ft_agl =
    weather->sky_condition[2].cloud_base_ft_agl =
    weather->sky_condition[3].cloud_base_ft_agl = -1;
  weather->sky_condition[0].sky_cover =
    weather->sky_condition[1].sky_cover =
    weather->sky_condition[2].sky_cover =
    weather->sky_condition[3].sky_cover = METAR_SKYCOND_UNKNOWN;
}

size_t xmlGetMetarCount(xmlDoc *xml)
{
  xmlXPathContext *xpath;
  xmlXPathObject *expr;
  size_t val;

  if ( !xml ) return 0;

  xpath = xmlXPathNewContext(xml);
  if ( !xpath ) return 0;

  expr = xmlXPathEvalExpression("count(//response/data/METAR)", xpath);
  xmlXPathFreeContext(xpath);

  if ( !expr ) return 0;
  val = (size_t)expr->floatval;
  xmlXPathFreeObject(expr);

  return val;
}

int xmlToMetar(xmlDoc *restrict xml, struct metar *restrict w, size_t count)
{
  size_t i;
  xmlNode *cur, *inner;
  xmlXPathContext *xpath;
  xmlXPathObject *expr;
  xmlAttr *xattr;
  xmlChar *xstr, *xstrInner;
  struct metar *weather;
  struct tm when;

  weather = w;

      if ( !xml )
      {
        //printf("No weather information for %s: invalid XML data.\n", argv[i]);
        return -1;
      }

      xpath = xmlXPathNewContext(xml);
      if ( !xpath )
      {
        //printf("No weather information for %s: invalid XPath context.\n", argv[i]);
        return -2;
      }

      // compile our weather information
      expr = xmlXPathEvalExpression("//response/data/METAR", xpath);
      xmlXPathFreeContext(xpath);

      if ( !expr )
      {
        //printf("No weather information for %s: invalid XPath expression.\n", argv[i]);
        return -3;
      }

      if ( xmlXPathNodeSetIsEmpty(expr->nodesetval) )
      {
        //printf("No weather information for %s is available at this time.\n", argv[i]);
        return 0;
      }

      for ( i = 0; (i < expr->nodesetval->nodeNr) && (i < count); ++i )
      {
        // all elements directly contain text except sky_condition,
        // which has the sky_cover and cloud_base_ft_agl attributes.
        if ( strcmp(expr->nodesetval->nodeTab[i]->name, "METAR") == 0 )
        {
          init_metar(weather);
          for ( cur = expr->nodesetval->nodeTab[i]->children;
                cur;
                cur = cur->next )
          {
            if ( cur->type != XML_ELEMENT_NODE ) continue;

            xstr = xmlNodeGetContent(cur);
            if ( strcmp(cur->name, "sky_condition") == 0 )
            {
              // <sky_condition sky_cover="string" cloud_base_ft_agl="string"/>
              xattr = cur->properties;
              while ( xattr && xattr->name && xattr->children && (weather->sky_condition_count < 4) )
              {
                xstrInner = xmlNodeGetContent(xattr->children);
                if ( strcmp(xattr->name, "sky_cover") == 0 )
                {
                  if ( strcmp(xstrInner, "SKC") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_SKC;
                  else if ( strcmp(xstrInner, "CLR") == 0 )
                    weather->sky_condition[weather->sky_condition_count++].sky_cover = METAR_SKYCOND_CLR;
                  else if ( strcmp(xstrInner, "CAVOK") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_CAVOK;
                  else if ( strcmp(xstrInner, "FEW") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_FEW;
                  else if ( strcmp(xstrInner, "SCT") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_SCT;
                  else if ( strcmp(xstrInner, "BKN") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_BKN;
                  else if ( strcmp(xstrInner, "OVC") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_OVC;
                  else if ( strcmp(xstrInner, "OVX") == 0 )
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_OVX;
                  else
                    weather->sky_condition[weather->sky_condition_count].sky_cover = METAR_SKYCOND_UNKNOWN;
                }
                else if ( strcmp(xattr->name, "cloud_base_ft_agl") == 0 )
                {
                  weather->sky_condition[weather->sky_condition_count++].cloud_base_ft_agl = atoi(xstrInner);
                }
                xmlFree(xstrInner); // inside a loop, unfortunately

                xattr = xattr->next;
              }
            }
            else if ( strcmp(cur->name, "raw_text") == 0 )
            {
              // <raw_text>string</raw_text>
              strncpy(weather->raw_text, xstr, METAR_BUFSIZE);
              weather->raw_text[METAR_BUFSIZE - 1] = '\0';
            }
            else if ( strcmp(cur->name, "station_id") == 0 )
            {
              // <station_id>char(4)</station_id>
              strncpy(weather->station_id, xstr, 4);
              weather->station_id[4] = '\0';
            }
            else if ( strcmp(cur->name, "observation_time") == 0 )
            {
              // <observation_time>ISO8601 string</observation_time>

              //struct tm when;
              if ( strptime(xstr, "%Y-%m-%dT%H:%M:%SZ", &when) != NULL )
                weather->observation_time = timegm(&when); //mktime(&when);

              //if ( getdate_r(cur->content, &when) == 0 )   // damn, doesn't work on OS X!
              //  weather->observation_time = mktime(&when);

              //when = getdate(xstr);
              //if ( when )
              //  weather->observation_time = mktime(when);
            }
            else if ( strcmp(cur->name, "latitude") == 0 )
            {
              // <latitude>float</latitude>
              weather->latitude = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "longitude") == 0 )
            {
              // <longitude>float</longitude>
              weather->longitude = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "temp_c") == 0 )
            {
              // <temp_c>float</temp_c>
              weather->temp_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "dewpoint_c") == 0 )
            {
              // <dewpoint_c>float</dewpoint_c>
              weather->dewpoint_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "wind_dir_degrees") == 0 )
            {
              // <wind_dir_degrees>int</wind_dir_degrees>
              weather->wind_dir_degrees = atoi(xstr);
            }
            else if ( strcmp(cur->name, "wind_speed_kt") == 0 )
            {
              // <wind_speed_kt>int</wind_speed_kt>
              weather->wind_speed_kt = atoi(xstr);
            }
            else if ( strcmp(cur->name, "wind_gust_kt") == 0 )
            {
              // <wind_gust_kt>int</wind_gust_kt>
              weather->wind_gust_kt = atoi(xstr);
            }
            else if ( strcmp(cur->name, "visibility_statute_mi") == 0 )
            {
              // <visibility_statue_mi>float</visibility_statue_mi>
              weather->visibility_statute_mi = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "altim_in_hg") == 0 )
            {
              // <altim_in_hg>float</altim_in_hg>
              weather->altim_in_hg = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "sea_level_pressure_mb") == 0 )
            {
              // <sea_level_pressure_mb>float</sea_level_pressure_mb>
              weather->sea_level_pressure_mb = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "quality_control_flags") == 0 )
            {
              // <quality_control_flags>
              //  <corrected>bool</corrected>
              //  <auto>bool</auto>
              //  <auto_station>bool</auto_station>
              //  <maintenance_indicator>bool?</maintenance_indicator>
              //  <no_signal>bool</no_signal>
              //  <lightning_sensor_off>bool</lightning_sensor_off>
              //  <freezing_rain_sensor_off>bool</freezing_rain_sensor_off>
              //  <present_weather_sensor_off>bool</present_weather_sensor_off>
              // </quality_control_flags>
              for ( inner = cur->children; inner; inner = inner->next )
              {
                xstrInner = xmlNodeGetContent(inner);
                if ( strcasecmp(xstrInner, "TRUE") != 0 )
                {
                  xmlFree(xstrInner);
                  continue;
                }
                xmlFree(xstrInner);

                if ( strcmp(inner->name, "corrected") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_CORRECTED;
                else if ( strcmp(inner->name, "auto") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_AUTO;
                else if ( strcmp(inner->name, "auto_station") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_AUTO_STATION;
                else if ( strcmp(inner->name, "maintenance_indicator") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_MAINTENANCE;
                else if ( strcmp(inner->name, "no_signal") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_NO_SIGNAL;
                else if ( strcmp(inner->name, "lightning_sensor_off") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_NO_LIGHTNING;
                else if ( strcmp(inner->name, "freezing_rain_sensor_off") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_NO_FREEZING;
                else if ( strcmp(inner->name, "present_weather_sensor_off") == 0 )
                  weather->quality_control_flags |= METAR_QUALITY_NO_WEATHER;
              }
            }
            else if ( strcmp(cur->name, "wx_string") == 0 )
            {
              // <wx_string>unknown</wx_string>
              strncpy(weather->wx_string, xstr, METAR_TINYBUFSIZE);
              weather->wx_string[METAR_TINYBUFSIZE - 1] = '\0';
            }
            else if ( strcmp(cur->name, "flight_category") == 0 )
            {
              // <flight_category>string</flight_category>
              if ( strcmp(xstr, "VFR") == 0 )
                weather->flight_category = METAR_CATEGORY_VFR;
              else if ( strcmp(xstr, "MVFR") == 0 )
                weather->flight_category = METAR_CATEGORY_MVFR;
              else if ( strcmp(xstr, "IFR") == 0 )
                weather->flight_category = METAR_CATEGORY_IFR;
              else if ( strcmp(xstr, "LIFR") == 0 )
                weather->flight_category = METAR_CATEGORY_LIFR;
              else
                weather->flight_category = METAR_CATEGORY_UNKNOWN;
            }
            else if ( strcmp(cur->name, "three_hr_pressure_tendency_mb") == 0 )
            {
              // <three_hr_pressure_tendency_mb>float</three_hr_pressure_tendency_mb>
              weather->three_hr_pressure_tendency_mb = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "maxT_c") == 0 )
            {
              // <maxT_c>float</maxT_c>
              weather->maxT_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "minT_c") == 0 )
            {
              // <minT_c>float</minT_c>
              weather->minT_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "maxT24hr_c") == 0 )
            {
              // <maxT24hr_c>float</maxT24hr_c>
              weather->maxT24hr_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "minT24hr_c") == 0 )
            {
              // <minT24hr_c>float</minT24hr_c>
              weather->minT24hr_c = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "precip_in") == 0 )
            {
              // <precip_in>float</precip_in>
              weather->precip_in = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "pcp3hr_in") == 0 )
            {
              // <pcp3hr_in>float</pcp3hr_in>
              weather->pcp3hr_in = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "pcp6hr_in") == 0 )
            {
              // <pcp6hr_in>float</pcp6hr_in>
              weather->pcp6hr_in = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "pcp24hr_in") == 0 )
            {
              // <pcp24hr_in>float</pcp24hr_in>
              weather->pcp24hr_in = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "snow_in") == 0 )
            {
              // <snow_in>float</snow_in>
              weather->snow_in = (float)atof(xstr);
            }
            else if ( strcmp(cur->name, "vert_vis_ft") == 0 )
            {
              // <vert_vis_ft>int</vert_vis_ft>
              weather->vert_vis_ft = atoi(xstr);
            }
            else if ( strcmp(cur->name, "metar_type") == 0 )
            {
              // <metar_type>string</metar_type>
              if ( strcmp(xstr, "METAR") == 0 )
                weather->metar_type = METAR_TYPE_METAR;
              else if ( strcmp(xstr, "SPECI") == 0 )
                weather->metar_type = METAR_TYPE_SPECI;
              else
                weather->metar_type = METAR_TYPE_UNKNOWN;
            }
            else if ( strcmp(cur->name, "elevation_m") == 0 )
            {
              // <elevation_m>float</elevation_m>
              weather->elevation_m = (float)atof(xstr);
            }

            if ( xstr != NULL )
            {
              xmlFree(xstr);
              xstr = NULL;
            }
          }
          ++weather;
        }
      }

  xmlXPathFreeObject(expr);
  return i;
}

void strReplaceTime(const char *restrict needle, time_t replacement, const char *restrict haystack, char *dest, size_t len)
{
  char buf[METAR_BUFSIZE];
  if ( replacement != 0 )
  {
    strftime(buf, METAR_BUFSIZE, "%Y-%m-%d %H:%M:%S (local)", localtime(&replacement));
    strReplace(needle, buf, haystack, dest, len);
  }
  else
  {
    strReplace(needle, "(unknown)", haystack, dest, len);
  }
}

void strReplaceTimeZulu(const char *restrict needle, time_t replacement, const char *restrict haystack, char *dest, size_t len)
{
  char buf[METAR_BUFSIZE];
  if ( replacement != 0 )
  {
    strftime(buf, METAR_BUFSIZE, "%Y-%m-%d %H:%M:%S (UTC)", gmtime(&replacement));
    strReplace(needle, buf, haystack, dest, len);
  }
  else
  {
    strReplace(needle, "(unknown)", haystack, dest, len);
  }
}

void strReplaceInt(const char *restrict needle, int replacement, const char *restrict haystack, char *dest, size_t len)
{
  char buf[METAR_REPLACEBUF];
  if ( replacement >= 0 )
  {
    snprintf(buf, METAR_REPLACEBUF, "%d", replacement);
    buf[METAR_REPLACEBUF - 1] = '\0';
    strReplace(needle, buf, haystack, dest, len);
  }
  else
  {
    strReplace(needle, "(unknown)", haystack, dest, len);
  }
}

void strReplaceFloat(const char *restrict needle, float replacement, const char *restrict haystack, char *dest, size_t len)
{
  char buf[METAR_REPLACEBUF * 2];
  if ( !isnan(replacement) )
  {
    snprintf(buf, METAR_REPLACEBUF * 2, "%.1f", roundf(replacement * 10.0f) / 10.0f);
    buf[(METAR_REPLACEBUF * 2) - 1] = '\0';
    strReplace(needle, buf, haystack, dest, len);
  }
  else
  {
    strReplace(needle, "(unknown)", haystack, dest, len);
  }
}

void strReplaceFloat2(const char *restrict needle, float replacement, const char *restrict haystack, char *dest, size_t len)
{
  char buf[METAR_REPLACEBUF * 2];
  if ( !isnan(replacement) )
  {
    snprintf(buf, METAR_REPLACEBUF * 2, "%.2f", roundf(replacement * 100.0f) / 100.0f);
    buf[(METAR_REPLACEBUF * 2) - 1] = '\0';
    strReplace(needle, buf, haystack, dest, len);
  }
  else
  {
    strReplace(needle, "(unknown)", haystack, dest, len);
  }
}

void strReplace(const char *restrict needle, const char *restrict replacement, const char *restrict haystack, char *dest, size_t len)
{
  size_t haystackLength, needleLength, replacementLength, pos;
  int i;

  needleLength = strlen(needle);
  haystackLength = strlen(haystack);
  replacementLength = strlen(replacement);

  pos = 0;

  for ( i = 0; i <= haystackLength; ++i )
  {
    if ( (i <= (haystackLength - needleLength)) && (strncmp(&haystack[i], needle, needleLength) == 0) )
    {
      if ( (pos + replacementLength) < len )
      {
        strncpy(&dest[pos], replacement, replacementLength);
        pos += replacementLength;
        i += needleLength - 1;
      }
    }
    else
    {
      if ( (pos + 1) < len )
      {
        dest[pos] = haystack[i];
        ++pos;
      }
    }
  }

  dest[pos] = '\0';
}

const char *skyCondition(enum sky_cover_type ceil)
{
  switch ( ceil )
  {
    case METAR_SKYCOND_BKN: return "Broken clouds";
    case METAR_SKYCOND_CAVOK: return "Ceiling/visibility okay";
    case METAR_SKYCOND_FEW: return "Few clouds";
    case METAR_SKYCOND_OVC: return "Overcast";
    case METAR_SKYCOND_OVX: return "Sky obscured";
    case METAR_SKYCOND_SCT: return "Scattered clouds";
    case METAR_SKYCOND_SKC: return "Sky clear";
    default: return "Unknown";
  }
}

int isVfrWeather(enum sky_cover_type ceil)
{
  if ( (ceil == METAR_SKYCOND_CAVOK) || (ceil == METAR_SKYCOND_FEW) || (ceil == METAR_SKYCOND_SCT)
    || (ceil == METAR_SKYCOND_SKC) || (ceil == METAR_SKYCOND_CLR) )
    return 1;
  return 0;
}

