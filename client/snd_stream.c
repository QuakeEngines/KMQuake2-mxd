/*
Copyright (C) 1997-2001 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// snd_stream.c -- Ogg Vorbis stuff

#include "client.h"
#include "snd_loc.h"

#ifdef OGG_SUPPORT

#define BUFFER_SIZE		16384
#define MAX_OGGLIST		512

static bgTrack_t s_bgTrack;
static channel_t *s_streamingChannel;

static qboolean ogg_started = false;// Initialization flag
static ogg_status_t ogg_status;		// Status indicator

static char **ogg_filelist;	// List of Ogg Vorbis files
static int ogg_numfiles;	// Number of Ogg Vorbis files
static int ogg_loopcounter;

static cvar_t *ogg_loopcount;
static cvar_t *ogg_ambient_track;

#pragma region ======================= Ogg vorbis streaming

static size_t ovc_read(void *ptr, size_t size, size_t nmemb, void *datasource)
{
	bgTrack_t *track = (bgTrack_t *)datasource;

	if (!size || !nmemb)
		return 0;

	if (track->filehandle)
		return FS_Read(ptr, size * nmemb, track->filehandle) / size;

	//mxd. GOG music tracks support...
	return fread(ptr, 1, size * nmemb, track->file) / size;
}

static int ovc_seek(void *datasource, ogg_int64_t offset, int whence)
{
	bgTrack_t *track = (bgTrack_t *)datasource;
	
	int fs_whence;
	switch (whence)
	{
		case SEEK_SET: fs_whence = FS_SEEK_SET; break;
		case SEEK_CUR: fs_whence = FS_SEEK_CUR; break;
		case SEEK_END: fs_whence = FS_SEEK_END; break;
		default: return -1;
	}

	if (track->filehandle) 
		FS_Seek(track->filehandle, (int)offset, fs_whence);
	else //mxd. GOG music tracks support...
		fseek(track->file, (int)offset, whence);

	return 0;
}

static int ovc_close(void *datasource)
{
	return 0;
}

static long ovc_tell(void *datasource)
{
	bgTrack_t *track = (bgTrack_t *)datasource;
	return (track->filehandle ? FS_Tell(track->filehandle) : ftell(track->file)); //mxd. GOG music tracks support...
}

#pragma endregion

#pragma region ======================= Ogg vorbis playback

static qboolean S_OpenBackgroundTrack(const char *name, bgTrack_t *track)
{
	const ov_callbacks vorbiscallbacks = { ovc_read, ovc_seek, ovc_close, ovc_tell };

	FS_FOpenFile(name, &track->filehandle, FS_READ);
	if(!track->filehandle) //mxd. GOG music tracks support...
		track->file = fopen(name, "rb");

	if (!track->filehandle && !track->file)
	{
		Com_Printf(S_COLOR_YELLOW"S_OpenBackgroundTrack: couldn't find '%s'\n", name);
		return false;
	}

	track->vorbisFile = Z_Malloc(sizeof(OggVorbis_File));

	// Bombs out here- ovc_read, FS_Read 0 bytes error
	if (ov_open_callbacks(track, track->vorbisFile, NULL, 0, vorbiscallbacks) < 0)
	{
		Com_Printf(S_COLOR_YELLOW"S_OpenBackgroundTrack: couldn't open OGG stream (%s)\n", name);
		return false;
	}

	vorbis_info *vorbisInfo = ov_info(track->vorbisFile, -1);
	if (vorbisInfo->channels != 1 && vorbisInfo->channels != 2)
	{
		Com_Printf(S_COLOR_YELLOW"S_OpenBackgroundTrack: only mono and stereo OGG files supported (%s)\n", name);
		return false;
	}

	track->start = ov_raw_tell(track->vorbisFile);
	track->rate = vorbisInfo->rate;
	track->width = 2;
	track->channels = vorbisInfo->channels; // Knightmare added

	return true;
}

static void S_CloseBackgroundTrack(bgTrack_t *track)
{
	if (track->vorbisFile)
	{
		ov_clear(track->vorbisFile);
		Z_Free(track->vorbisFile);
		track->vorbisFile = NULL;
	}

	if (track->filehandle)
	{
		FS_FCloseFile(track->filehandle);
		track->filehandle = 0;
	}
	else if(track->file) //mxd. GOG music tracks support...
	{
		fclose(track->file);
		track->file = NULL;
	}
}

static void S_StreamBackgroundTrack(void)
{
	int dummy;
	byte data[MAX_RAW_SAMPLES * 4];

	if ((!s_bgTrack.filehandle && !s_bgTrack.file) || !s_musicvolume->value || !s_streamingChannel) //mxd. GOG music tracks support...
		return;

	s_rawend = max(paintedtime, s_rawend);

	const float scale = (float)s_bgTrack.rate / dma.speed;
	const int maxSamples = sizeof(data) / s_bgTrack.channels / s_bgTrack.width;

	while (true)
	{
		int samples = (paintedtime + MAX_RAW_SAMPLES - s_rawend) * scale;
		if (samples <= 0)
			return;

		samples = min(maxSamples, samples);
		const int maxRead = samples * s_bgTrack.channels * s_bgTrack.width;

		int total = 0;
		while (total < maxRead)
		{
			const int read = ov_read(s_bgTrack.vorbisFile, (char *)data + total, maxRead - total, 0, 2, 1, &dummy);
			if (!read)
			{
				// End of file
				if (!s_bgTrack.looping)
				{
					// Close the intro track
					S_CloseBackgroundTrack(&s_bgTrack);

					// Open the loop track
					if (!S_OpenBackgroundTrack(s_bgTrack.loopName, &s_bgTrack))
					{
						S_StopBackgroundTrack();
						return;
					}

					s_bgTrack.looping = true;
				}
				else
				{
					// Check if it's time to switch to the ambient track //mxd. Also check that ambientName contains data
					if (s_bgTrack.ambientName[0] && ++ogg_loopcounter >= ogg_loopcount->integer && (!cl.configstrings[CS_MAXCLIENTS][0] || !strcmp(cl.configstrings[CS_MAXCLIENTS], "1")))
					{
						// Close the loop track
						S_CloseBackgroundTrack(&s_bgTrack);

						if (!S_OpenBackgroundTrack(s_bgTrack.ambientName, &s_bgTrack) && !S_OpenBackgroundTrack(s_bgTrack.loopName, &s_bgTrack))
						{
							S_StopBackgroundTrack();
							return;
						}

						s_bgTrack.ambient_looping = true;
					}
				}

				// Restart the track, skipping over the header
				ov_raw_seek(s_bgTrack.vorbisFile, (ogg_int64_t)s_bgTrack.start);
			}

			total += read;
		}

		S_RawSamples(samples, s_bgTrack.rate, s_bgTrack.width, s_bgTrack.channels, data, true);
	}
}

// Streams background track
void S_UpdateBackgroundTrack(void)
{
	// Stop music if paused
	if (ogg_status == PLAY)
		S_StreamBackgroundTrack();
}

void S_StartBackgroundTrack(const char *introTrack, const char *loopTrack)
{
	if (!ogg_started) // Was sound_started
		return;

	// Stop any playing tracks
	S_StopBackgroundTrack();

	// Start it up
	Q_strncpyz(s_bgTrack.introName, introTrack, sizeof(s_bgTrack.introName));
	Q_strncpyz(s_bgTrack.loopName, loopTrack, sizeof(s_bgTrack.loopName));

	//mxd. No, we don't want to play "music/.ogg"
	if (ogg_ambient_track->string[0])
		Q_strncpyz(s_bgTrack.ambientName, va("music/%s.ogg", ogg_ambient_track->string), sizeof(s_bgTrack.ambientName));
	else
		s_bgTrack.ambientName[0] = '\0';

	// Set a loop counter so that this track will change to the ambient track later
	ogg_loopcounter = 0;

	S_StartStreaming();

	// Open the intro track
	if (!S_OpenBackgroundTrack(s_bgTrack.introName, &s_bgTrack))
	{
		S_StopBackgroundTrack();
		return;
	}

	ogg_status = PLAY;

	S_StreamBackgroundTrack();
}

void S_StopBackgroundTrack(void)
{
	if (!ogg_started) // Was sound_started
		return;

	S_StopStreaming();
	S_CloseBackgroundTrack(&s_bgTrack);

	ogg_status = STOP;

	memset(&s_bgTrack, 0, sizeof(bgTrack_t));
}

void S_StartStreaming(void)
{
	if (!ogg_started || s_streamingChannel) // Was sound_started || already started
		return;

	s_streamingChannel = S_PickChannel(0, 0);
	if (!s_streamingChannel)
		return;

	s_streamingChannel->streaming = true;
}

void S_StopStreaming(void)
{
	if (!ogg_started || !s_streamingChannel) // Was sound_started || already stopped
		return;

	s_streamingChannel->streaming = false;
	s_streamingChannel = NULL;
}

#pragma endregion

#pragma region ======================= Init / shutdown / restart / list files

// Initialize the Ogg Vorbis subsystem
// Based on code by QuDos
void S_OGG_Init(void)
{
	static qboolean ogg_first_init = true; //mxd. Made local
	
	if (ogg_started)
		return;

	// Cvars
	ogg_loopcount = Cvar_Get("ogg_loopcount", "5", CVAR_ARCHIVE);
	ogg_ambient_track = Cvar_Get("ogg_ambient_track", "", CVAR_ARCHIVE); //mxd. Default value was "track11"

	// Console commands
	Cmd_AddCommand("ogg", S_OGG_ParseCmd);

	// Build list of files
	Com_Printf("Searching for Ogg Vorbis files...\n");
	ogg_numfiles = 0;
	S_OGG_LoadFileList();
	Com_Printf("%d Ogg Vorbis files found.\n", ogg_numfiles);

	// Initialize variables
	if (ogg_first_init)
	{
		ogg_status = STOP;
		ogg_first_init = false;
	}

	ogg_started = true;
}

// Shutdown the Ogg Vorbis subsystem
// Based on code by QuDos
void S_OGG_Shutdown(void)
{
	if (!ogg_started)
		return;

	S_StopBackgroundTrack();

	// Free the list of files
	FS_FreeFileList(ogg_filelist, MAX_OGGLIST); //mxd. Free unconditionally

	// Remove console commands
	Cmd_RemoveCommand("ogg");

	ogg_started = false;
}

// Reinitialize the Ogg Vorbis subsystem
// Based on code by QuDos
void S_OGG_Restart(void)
{
	S_OGG_Shutdown();
	S_OGG_Init();
}

//mxd
static void LoadDirectoryFileList(const char *path)
{
	char findname[MAX_OSPATH];
	int numfiles = 0;
	
	// Get file list
	Com_sprintf(findname, sizeof(findname), "%s/music/*.ogg", path);
	char **list = FS_ListFiles(findname, &numfiles, 0, SFF_SUBDIR | SFF_HIDDEN | SFF_SYSTEM);

	// Add valid Ogg Vorbis file to the list
	for (int i = 0; i < numfiles && ogg_numfiles < MAX_OGGLIST; i++)
	{
		if (!list[i])
			continue;

		char *p = list[i];

		if (!strstr(p, ".ogg"))
			continue;

		if (!FS_ItemInList(p, ogg_numfiles, ogg_filelist)) // Check if already in list
		{
			ogg_filelist[ogg_numfiles] = malloc(strlen(p) + 1);
			sprintf(ogg_filelist[ogg_numfiles], "%s", p);
			ogg_numfiles++;
		}
	}

	if (numfiles) // Free the file list
		FS_FreeFileList(list, numfiles);
}

// Load list of Ogg Vorbis files in music/
// Based on code by QuDos
void S_OGG_LoadFileList(void)
{
	ogg_filelist = malloc(sizeof(char *) * MAX_OGGLIST);
	memset(ogg_filelist, 0, sizeof(char *) * MAX_OGGLIST);

	// Check search paths
	char *path = NULL;
	while ((path = FS_NextPath(path)) != NULL)
		LoadDirectoryFileList(path);

	//mxd. The GOG version of Quake2 has the music tracks in Quake2/music/TrackXX.ogg, so let's check for those as well...
	LoadDirectoryFileList(fs_basedir->string);

	// Check paks
	int numfiles = 0;
	char **list = FS_ListPak("music/", &numfiles);
	if (list)
	{
		// Add valid Ogg Vorbis file to the list
		for (int i = 0; i < numfiles && ogg_numfiles < MAX_OGGLIST; i++)
		{
			if (!list[i])
				continue;

			char *p = list[i];

			if (!strstr(p, ".ogg"))
				continue;

			if (!FS_ItemInList(p, ogg_numfiles, ogg_filelist)) // Check if already in list
			{
				ogg_filelist[ogg_numfiles] = malloc(strlen(p) + 1);
				sprintf(ogg_filelist[ogg_numfiles], "%s", p);
				ogg_numfiles++;
			}
		}
	}

	if (numfiles)
		FS_FreeFileList(list, numfiles);
}

#pragma endregion

#pragma region ======================= Console commands

// Based on code by QuDos
static void S_OGG_PlayCmd(void)
{
	if (Cmd_Argc() < 3)
	{
		Com_Printf("Usage: ogg play <track>\n");
		return;
	}

	char name[MAX_QPATH];
	Com_sprintf(name, sizeof(name), "music/%s.ogg", Cmd_Argv(2));
	S_StartBackgroundTrack(name, name);
}

// Based on code by QuDos
static void S_OGG_StatusCmd(void)
{
	char *trackName;

	if (s_bgTrack.ambient_looping)
		trackName = s_bgTrack.ambientName;
	else if (s_bgTrack.looping)
		trackName = s_bgTrack.loopName;
	else
		trackName = s_bgTrack.introName;

	switch (ogg_status)
	{
		case PLAY:
			Com_Printf("Playing file %s at %0.2f seconds.\n", trackName, ov_time_tell(s_bgTrack.vorbisFile));
			break;

		case PAUSE:
			Com_Printf("Paused file %s at %0.2f seconds.\n", trackName, ov_time_tell(s_bgTrack.vorbisFile));
			break;

		case STOP:
			Com_Printf("Stopped.\n");
			break;
	}
}

// List Ogg Vorbis files
// Based on code by QuDos
static void S_OGG_ListCmd(void)
{
	if (ogg_numfiles <= 0)
	{
		Com_Printf(S_COLOR_GREEN"No Ogg Vorbis files to list.\n");
		return;
	}

	for (int i = 0; i < ogg_numfiles; i++)
		Com_Printf("%d: %s\n", i + 1, ogg_filelist[i]);

	Com_Printf(S_COLOR_GREEN"%d Ogg Vorbis files.\n", ogg_numfiles);
}

// Parses OGG commands
// Based on code by QuDos
void S_OGG_ParseCmd(void)
{
	if (Cmd_Argc() < 2)
	{
		Com_Printf("Usage: ogg { play | pause | resume | stop | status | list }\n");
		return;
	}

	char *command = Cmd_Argv(1);

	if (Q_strcasecmp(command, "play") == 0)
	{
		S_OGG_PlayCmd();
	}
	else if (Q_strcasecmp(command, "pause") == 0)
	{
		if (ogg_status == PLAY)
			ogg_status = PAUSE;
	}
	else if (Q_strcasecmp(command, "resume") == 0)
	{
		if (ogg_status == PAUSE)
			ogg_status = PLAY;
	}
	else if (Q_strcasecmp(command, "stop") == 0)
	{
		S_StopBackgroundTrack();
	}
	else if (Q_strcasecmp(command, "status") == 0)
	{
		S_OGG_StatusCmd();
	}
	else if (Q_strcasecmp(command, "list") == 0)
	{
		S_OGG_ListCmd();
	}
	else
	{
		Com_Printf("Usage: ogg { play | pause | resume | stop | status | list }\n");
	}
}

#pragma endregion

#endif // OGG_SUPPORT