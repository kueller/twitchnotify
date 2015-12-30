#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <curl/curl.h>
#include <libnotify/notify.h>
#include <gdk-pixbuf/gdk-pixbuf.h>

#define STREAM_OFFLINE        0
#define STREAM_ONLINE         1
#define STREAM_404           -1

#define NODAEMON_FLAG     0b001
#define BROWSER_FLAG      0b010
#define LIVESTREAMER_FLAG 0b100

#define COUNT_BUFFER 4

struct stream {
	char *name;
	char *game;

	CURL *statcurl;
	CURL *gamecurl;

	NotifyNotification *note;

	int status;
	int count;
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
// Callback for stream_is_online()
// I can't into string parsing in C
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

			if (!strcmp(token, "{\"stream\":null")) {
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
// Callback for current_game()
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

// Callback functions for calling system() to open in browser or livestreamer.
// These don't work when running as a daemon for some reason.
void notify_open_browser(NotifyNotification *n, const char *action,
						 gpointer user_data)
{
	Stream stream = (Stream)user_data;
	
	char command[100];
	sprintf(command, "xdg-open http://www.twitch.tv/%s", stream->name);
	
	system(command);
}

void notify_open_livestreamer(NotifyNotification *n, const char *action,
							  gpointer user_data)
{
	Stream stream = (Stream)user_data;
	
	char command[200];
	sprintf(command, "livestreamer http://www.twitch.tv/%s best &",
			stream->name);

	system(command);
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
		twitch_notify_exit("Invalid stream name.");
	}

	return status;
}

// Puts the current game (if any) into the "game" variable in a Stream.
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

// Sets the GTK notification action buttons.
void set_notification_actions(Stream s, int options)
{
	if (!s) return;
	if (options == 0) return;

	if (options & LIVESTREAMER_FLAG) {
		notify_notification_add_action(s->note, "livestr-open",
									   "Open with livestreamer",
									   NOTIFY_ACTION_CALLBACK(notify_open_livestreamer),
									   s, NULL);
	}

	if (options & BROWSER_FLAG) {
		notify_notification_add_action(s->note, "browser-open",
									   "Open in browser",
									   NOTIFY_ACTION_CALLBACK(notify_open_browser),
									   s, NULL);
	}
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

	char url[70];
	sprintf(url, "https://api.twitch.tv/kraken/streams/%s", streamer);

	curl_easy_setopt(c, CURLOPT_URL, url);
	curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, check_stream);

	return c;
}

CURL *game_request_init(char *streamer)
{
	CURL *c = curl_easy_init();
	if (!c) twitch_notify_exit("Failed to initialize URL object.");

	char url[70];
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
	s->count  = 0;
	
	s->note = notification_init();

	s->statcurl = status_request_init(s->name);
	s->gamecurl = game_request_init(s->name);

	return s;
}

void send_twitch_notification(Stream s)
{
	get_current_game(s);

	char message[100];
	sprintf(message, "%s is online.\nPlaying: %s",
			s->name, strlen(s->game) ? s->game : "");

	notify_notification_update(s->note, "Twitch Notify", message, NULL);
	notify_notification_show(s->note, NULL);
}

// Callback function. Called every 15 seconds.
// Notification is sent if a stream is online for 3 passes.
// Similarly, status changes to offline is stream is offline for 3 passes.
gboolean check_all_streams(gpointer user_data)
{
	int newstat = STREAM_OFFLINE;
	int i;

	Stream *stream_list = (Stream *)user_data;
	for (i = 0; stream_list[i]; i++) {
		Stream current = stream_list[i];

		newstat = stream_is_online(current);

		if (newstat == STREAM_ONLINE && current->status == STREAM_OFFLINE) {
			if (current->count == COUNT_BUFFER) {
				send_twitch_notification(current);
				current->status = STREAM_ONLINE;
				current->count = 0;
			} else if (current->count < COUNT_BUFFER) {
				current->count++;
			}
		} else if (newstat == STREAM_OFFLINE &&
				   current->status == STREAM_ONLINE) {
			if (current->count == COUNT_BUFFER) {
				current->status = STREAM_OFFLINE;
				current->count = 0;
			} else if (current->count < COUNT_BUFFER) {
				current->count++;
			}
		} else if (newstat == current->status) {
			current->count = 0;
		}
	}

	return TRUE;
}	

int main(int argc, char **argv)
{
	if (argc < 2) twitch_notify_exit("Need arguments!\n"
									 "Try using the --help option.");
	
	if (!strcmp(argv[1], "--help")) {
		printf("Usage: twitchnotify [OPTION] [STREAMERS]\n\n"
			   "Pass in a list of streamer you would like to keep track of.\n"
			   "Allows for up to 100 different streams.\n"
			   "Use --no-daemon to prevent from forking.\n\n"
			   "You can try --browser and --livestreamer options to get\n"
			   "buttons in the notification bubble. These do not work when\n"
			   "the program is run as a daemon though, so consider it\n"
			   "experimental for now.\n\n");
			   /*
			   "Options:\n"
			   "   --no-daemon\n"
			   "   Prevents from forking to the background.\n\n"
			   "   --browser\n"
			   "   Adds option to open stream in default browser.\n\n"
			   "   --livestreamer\n"
			   "   Adds option to open with livestreamer.\n"
			   "   Uses \"best\" quality option only, for now.\n\n");
			   */
		exit(0);
	}

	GMainLoop *loop = g_main_loop_new(NULL, FALSE);

	int options      = 0b0;
	int stream_count = 0;
	
	Stream stream_list[100];

	int i;
	for (i = 0; i < 100; i++) stream_list[i] = NULL;

	for (i = 1; i < argc; i++) {
		if (is_a_letter(argv[i][0])) {
			printf("Starting Twitch Notify for %s...\n", argv[i]);
			stream_list[stream_count] = stream_init(argv[i]);
			stream_is_online(stream_list[stream_count]);
			stream_count++;
		} else {
			if (!strcmp(argv[i], "--no-daemon"))
				options |= NODAEMON_FLAG;
			else if (!strcmp(argv[i], "--browser"))
				options |= BROWSER_FLAG;
			else if (!strcmp(argv[i], "--livestreamer"))
				options |= LIVESTREAMER_FLAG;
		}
	}

	if (stream_count < 1) twitch_notify_exit("Need stream name!");
	putchar('\n');
	putchar('\n');

	if (!(options & NODAEMON_FLAG)) {
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

	printf("Starting loop\n");

	g_timeout_add_seconds(10, check_all_streams, stream_list);
	g_main_loop_run(loop);

	return 0;
}
