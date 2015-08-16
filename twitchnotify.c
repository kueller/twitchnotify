#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <libnotify/notify.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define STREAM_OFFLINE  0
#define STREAM_ONLINE   1
#define STREAM_404     -1

struct stream {
	char *name;
	char *game;

	CURL *statcurl;
	CURL *gamecurl;

	int status;
};

typedef struct stream* Stream;

void twitch_notify_exit(char *errormsg)
{
	fprintf(stderr, "Error: %s\n\n", errormsg);

	if (notify_is_initted()) notify_uninit();
	exit(1);
}

int is_a_letter(char c)
{
	return ((c >= 65 && c <= 90) || (c >= 97 && c <= 122));
}

// status is a pointer to an int that will hold the stream's new status
// Callback for StreamIsOnline()
size_t check_stream(void *ptr, size_t size, size_t nmemb, void *status)
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
size_t find_game(void *ptr, size_t size, size_t nmemb, void *game)
{
	int i = 0;
	
	char **entries = (char **)calloc(35, sizeof(char *));
	if (!entries) twitch_notify_exit("Memory allocation failure.");

	char *token = strtok((char *)ptr, ",");
	for (i = 0; token && i < 35; i++) {
		entries[i] = (char *)calloc(strlen(token) + 1, sizeof(char));
		if (!entries[i]) twitch_notify_exit("Memory allocation failure.");
		
		strncpy(entries[i], token, strlen(token) + 1);
		token = strtok(NULL, ",");
	}

	for (i = 0; entries[i] && i < 35; i++) {
		token = strtok(entries[i], ":");

		// Looking for form "game":"Game Title"
		if (token && !strcmp(token, "\"game\"")) {
			token = strtok(NULL, ":");
			if (token) {
				strncpy((char *)game, token, strlen(token) + 1);
				token = strtok(NULL, ":");
				while (token) {
					strcat((char *)game, ":");
					strcat((char *)game, token);
					token = strtok(NULL, ":");
				}
				
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
	
int stream_is_online(Stream s)
{
	int status;

	CURLcode res;

	// Program will continue on errors unless stream 404s (invalid stream)
	res = curl_easy_setopt(s->statcurl, CURLOPT_WRITEDATA, &status);
	if (res != CURLE_OK) return s->status;

	res = curl_easy_perform(s->statcurl);
	if (res != CURLE_OK) return STREAM_OFFLINE;
	
	if (status == STREAM_404) {
		fprintf(stderr, "Stream: %s\n", s->name);
		twitch_notify_exit("Invalid stream name.");
	}

	return status;
}

// Returns allocated string, needs to be freed after.
void get_current_game(Stream s)
{
	int i;
	for (i = 0; i < 50; i++) s->game[i] = '\0';

	CURLcode res;
	res = curl_easy_setopt(s->gamecurl, CURLOPT_WRITEDATA, s->game);
	if (res != CURLE_OK) return;

	res = curl_easy_perform(s->gamecurl);
	if (res != CURLE_OK) return;
}

NotifyNotification *notification_init()
{
	notify_init("Twitch Notify");
	NotifyNotification *n;

	n = notify_notification_new("Twitch Notify", NULL, NULL);
	if (!n) twitch_notify_exit("Failed to initialize notification.");

	// Will stay up until clicked off
	notify_notification_set_timeout(n, NOTIFY_EXPIRES_NEVER);

	GdkPixbuf *icon = gdk_pixbuf_new_from_file(
		"/usr/share/twitchnotify/GlitchIcon_purple.png", 
		NULL);

	if (icon) {
		notify_notification_set_image_from_pixbuf(n, icon);
	}

	return n;
}

CURL *status_request_init(char *streamer)
{
	CURL *c = curl_easy_init();
	if (!c) twitch_notify_exit("Failed to initialize URL object.");

	char url[50];
	sprintf(url, "https://api.twitch.tv/kraken/streams/%s", streamer);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, check_stream);

	return c;
}

CURL *game_request_init(char *streamer)
{
	CURL *c = curl_easy_init();
	if (!c) twitch_notify_exit("Failed to initialize URL object.");

	char url[50];
	sprintf(url, "https://api.twitch.tv/kraken/channels/%s", streamer);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, find_game);

	return c;
}

Stream stream_init(char *streamer)
{
	Stream s = (Stream)malloc(sizeof(struct stream));
	if (!s) twitch_notify_exit("Memory allocation error");

	s->game = (char *)calloc(50, sizeof(char));
	if (!s->game) twitch_notify_exit("Memory allocation error");

	s->name   = streamer;
	s->status = STREAM_OFFLINE;

	s->statcurl = status_request_init(s->name);
	s->gamecurl = game_request_init(s->name);

	return s;
}

void send_twitch_notification(NotifyNotification *n, Stream s)
{
	get_current_game(s);

	char message[100];
	sprintf(message, "%s is online.\nPlaying: %s",
			s->name, strlen(s->game) ? s->game : "");

	notify_notification_update(n, "Twitch Notify", message, NULL);
	notify_notification_show(n, NULL);
}

int main(int argc, char **argv)
{
	if (argc < 2) twitch_notify_exit("Need arguments!");
	
	if (!strcmp(argv[1], "--help")) {
		printf("Usage: twitchnotify [OPTION] [STREAMERS]\n\n"
			   "Pass in a list of streamer you would like to keep track of.\n"
			   "Use --no-daemon to prevent forking to background.\n\n");
		exit(0);
	}
	
	int daemonize    = 1;
	int stream_count = 0;
	
	Stream stream_list[100];

	int i;
	for (i = 1; i < argc; i++) {
		if (!is_a_letter(argv[i][0])) {
			if (!strcmp(argv[i], "--no-daemon"))
				daemonize = 0;
		}
	}

	for (i = 1; i < argc; i++) {
		if (is_a_letter(argv[i][0])) {
			printf("Starting Twitch Notify for %s...\n", argv[i]);
			stream_list[stream_count] = stream_init(argv[i]);
			stream_count++;
		}
	}

	if (stream_count < 1) twitch_notify_exit("Need stream name!");
	putchar('\n');

	NotifyNotification *note = notification_init();

	printf("Getting initial statuses...\n");

	for (i = 0; i < stream_count; i++) {

		stream_list[i]->status = stream_is_online(stream_list[i]);
		if (stream_list[i]->status == STREAM_ONLINE) {
			get_current_game(stream_list[i]);
			printf("%s is online playing %s!\n", stream_list[i]->name,
				   strlen(stream_list[i]->game) ? stream_list[i]->game : "");
			send_twitch_notification(note, stream_list[i]);
		}
	}
	
	putchar('\n');

	if (daemonize) {
		printf("Forking to background...\n");

		pid_t pid, sid;
		pid = fork();
		if (pid < 0) twitch_notify_exit("Program forking failed.");
		else if (pid > 0) exit(0);

		umask(0);
		sid = setsid();
		if (sid < 0) twitch_notify_exit("Program SID setting failed.");

		if (chdir("/") < 0) twitch_notify_exit("Setting program directory.");

		close(STDIN_FILENO);
		close(STDOUT_FILENO);
		close(STDERR_FILENO);
	}

	int newstat = STREAM_OFFLINE;
	
	// Only sends notification on the rising offline->online transition
	// Checks every 30 seconds
	while (1) {
		for (i = 0; i < stream_count; i++) {
			Stream current = stream_list[i];

			newstat = stream_is_online(current);

			if (newstat == STREAM_ONLINE && current->status == STREAM_OFFLINE)
			{
				send_twitch_notification(note, current);
				current->status = STREAM_ONLINE;
			}

			else if (newstat == STREAM_OFFLINE && current->status == STREAM_ONLINE)
			{
				current->status = STREAM_OFFLINE;
			}
		}

		sleep(30);
	}

	return 0;
}
