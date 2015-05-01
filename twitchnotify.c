#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <libnotify/notify.h>

#define STREAM_OFFLINE  0
#define STREAM_ONLINE   1
#define STREAM_404     -1

// Current state of the program, or the "previous" status
static int STREAM_STATUS = STREAM_OFFLINE;

void TwitchNotifyExit(char *errormsg, CURL *status, CURL *game)
{
	fprintf(stderr, "Error: %s\n\n", errormsg);

	if (status) curl_easy_cleanup(status);
	if (game) curl_easy_cleanup(game);
	if (notify_is_initted()) notify_uninit();

	exit(1);
}

NotifyNotification *NotificationInit(char *streamer)
{
	notify_init("Twitch Notify");
	NotifyNotification *n;

	n = notify_notification_new("Twitch Notify", NULL, NULL);
	if (!n) TwitchNotifyExit("Failed to initialize notification.", NULL, NULL);

	// Will stay up until clicked off
	notify_notification_set_timeout(n, NOTIFY_EXPIRES_NEVER);

	return n;
}

void SendTwitchNotification(NotifyNotification *n, char *streamer, char *game)
{
	char message[100];
	sprintf(message, "%s is online.\nPlaying: %s", streamer, game ? game : "");

	notify_notification_update(n, "Twitch Notify", message, NULL);
	notify_notification_show(n, NULL);
}

// status is a pointer to an int that will hold the stream's new status
// Callback for StreamIsOnline()
size_t CheckStream(void *ptr, size_t size, size_t nmemb, void *status)
{
	char *token = strtok((char *)ptr, ",");
	
	if (!strncmp(token, "{\"error\":\"Not Found\"", 20)) {
		*(int *)status = STREAM_404;
	} else {
		int i;
		while (token) {

			// Clear the token string before reuse
			for (i = 0; token[i]; i++) {
				if (token[i] == '}')
					token[i] = '\0';
			}

			if (!strcmp(token, "\"stream\":null")) {
				*(int *)status = STREAM_OFFLINE;
				break;
			} else {
				*(int *)status = STREAM_ONLINE;
			}

			token = strtok(NULL, ",");
		}
	}

	return size * nmemb;
}

// Returns the game currently being played, as a string, into the game variable
// Callback for CurrentGame()
size_t FindGame(void *ptr, size_t size, size_t nmemb, void *game)
{
	char **entries = (char **)malloc(35 * sizeof(char *));
	if (!entries) TwitchNotifyExit("Memory allocation failure.", NULL, NULL);

	char *token = strtok((char *)ptr, ",");
	int i;
	for (i = 0; token && i < 35; i++) {
		entries[i] = (char *)calloc(strlen(token) + 1, sizeof(char));
		if (!entries[i]) TwitchNotifyExit("Memory allocation failure.", NULL, NULL);
		
		strcpy(entries[i], token);
		token = strtok(NULL, ",");
	}

	for (i = 0; entries[i] && i < 35; i++) {
		token = strtok(entries[i], ":");

		// Looking for form "game":"Game Title"
		if (token && !strcmp(token, "\"game\"")) {
			token = strtok(NULL, ":");
			if (token) {
				strncpy((char *)game, token, strlen(token));
				break;
			}
		}
	}

	for (i = 0; entries[i]; i++) {
		free(entries[i]);
	}

	free(entries);

	return size * nmemb;
}

CURL *StatusRequestInit(char *streamer)
{
	CURL *c = curl_easy_init();
	if (!c) TwitchNotifyExit("Failed to initialize URL object.", NULL, NULL);

	char url[50];
	sprintf(url, "https://api.twitch.tv/kraken/streams/%s", streamer);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, CheckStream);

	return c;
}

CURL *GameRequestInit(char *streamer)
{
	CURL *c = curl_easy_init();
	if (!c) TwitchNotifyExit("Failed to initialize URL object.", NULL, NULL);

	char url[50];
	sprintf(url, "https://api.twitch.tv/kraken/channels/%s", streamer);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, FindGame);

	return c;
}
	
int StreamIsOnline(CURL *curl)
{
	int status;

	CURLcode res;

	// Program will continue on errors unless stream 404s (invalid stream)
	res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, &status);
	if (res != CURLE_OK) return STREAM_OFFLINE;

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) return STREAM_OFFLINE;

	if (status == STREAM_404) TwitchNotifyExit("Invalid stream name.", curl, NULL);

	return status;
}

// Returns allocated string, needs to be freed after.
char *CurrentGame(CURL *curl)
{
	char *game = (char *)calloc(50, sizeof(char));
	if (!game) TwitchNotifyExit("Memory allocation failure.", NULL, curl);

	CURLcode res;
	res = curl_easy_setopt(curl, CURLOPT_WRITEDATA, game);
	if (res != CURLE_OK) {
		free(game);
		return NULL;
	}

	res = curl_easy_perform(curl);
	if (res != CURLE_OK) {
		free(game);
		return NULL;
	}

	return game;
}

int main(int argc, char **argv)
{
	if (argc != 2)
		TwitchNotifyExit("Must specify one valid Twitch stream name."
						 "\nUsage: twitchnotify [STREAMNAME]", NULL, NULL);

	char *streamer = argv[1];
	char *game     = NULL;

	printf("Starting Twitch Notify for %s...\n", streamer);

	NotifyNotification *n = NotificationInit(streamer);
	CURL *statuscurl      = StatusRequestInit(streamer);
	CURL *gamecurl        = GameRequestInit(streamer);

	printf("Getting initial status...\n");
	STREAM_STATUS = StreamIsOnline(statuscurl);

	if (STREAM_STATUS == STREAM_ONLINE) {
		game = CurrentGame(gamecurl);
		printf("%s is online playing %s!\n", streamer, game ? game : "");
		if (game) free(game);
		game = NULL;
	}
	
	printf("\nForking to background...\n\n");
	
	pid_t pid, sid;
	pid = fork();
	if (pid < 0) TwitchNotifyExit("Program forking failed.", statuscurl, gamecurl);
	else if (pid > 0) exit(0);

	umask(0);
	sid = setsid();
	if (sid < 0) TwitchNotifyExit("Program SID setting failed.", statuscurl, gamecurl);

	if (chdir("/") < 0) TwitchNotifyExit("Setting program directory.", statuscurl, gamecurl);

	close(STDIN_FILENO);
	close(STDOUT_FILENO);
	close(STDERR_FILENO);

	// Only sends notification on the rising offline->online transition
	// Checks every 30 seconds
	while (1) {

		if (StreamIsOnline(statuscurl) &&
			STREAM_STATUS == STREAM_OFFLINE)
		{
			game = CurrentGame(gamecurl);
			SendTwitchNotification(n, streamer, game);
			if (game) free(game);
			game = NULL;
			
			STREAM_STATUS = STREAM_ONLINE;
		}

		else if (!StreamIsOnline(statuscurl) &&
				   STREAM_STATUS == STREAM_ONLINE)
		{
			STREAM_STATUS = STREAM_OFFLINE;
		}

		sleep(30);
	}

	curl_easy_cleanup(statuscurl);
	curl_easy_cleanup(gamecurl);
	notify_uninit();
	
	return 0;
}
