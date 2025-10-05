#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ctype.h>
#include <signal.h>
#include <sys/stat.h>

#ifdef _WIN32
    #include <winsock2.h>
    #include <ws2tcpip.h>
    #include <windows.h>
    #pragma comment(lib, "ws2_32.lib")
    #define close closesocket
    typedef int socklen_t;
#else
    #include <unistd.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netdb.h>
    #include <arpa/inet.h>
    #include <termios.h>
    #include <fcntl.h>
    #include <pthread.h>
#endif

#define IRC_SERVER "irc.chat.twitch.tv"
#define IRC_PORT 6667
#define BUFFER_SIZE 4096
#define MAX_MESSAGES 50
#define MAX_USERNAME 100
#define MAX_MESSAGE 512
#define CONFIG_FILE ".twitch_config"
#define BLACKLIST_FILE "blacklist.txt"
#define HTML_FILE "twitchchat.html"
#define CSS_FILE "twitchchat.css"
#define MAX_BLACKLIST_WORDS 1000

typedef struct {
    char username[MAX_USERNAME];
    char message[MAX_MESSAGE];
    int is_broadcaster;
    int is_moderator;
    int is_subscriber;
    int is_vip;
    time_t timestamp;
} ChatMessage;

typedef struct {
    ChatMessage messages[MAX_MESSAGES];
    int count;
    pthread_mutex_t lock;
} ChatBuffer;

// Global variables (yeah, globals are kinda shit but fuck it, this works)
#ifdef _WIN32
SOCKET sockfd = INVALID_SOCKET;
#else
int sockfd = -1;
#endif

ChatBuffer chat_buffer = {.count = 0};
int running = 1;
int display_mode = 0; // 0 = terminal, 1 = web server
char target_channel[MAX_USERNAME];
char my_nickname[MAX_USERNAME];
char* blacklist_words[MAX_BLACKLIST_WORDS];
int blacklist_count = 0;

#ifdef _WIN32
HANDLE mutex;
#define pthread_mutex_t HANDLE
#define pthread_mutex_lock(m) WaitForSingleObject(*m, INFINITE)
#define pthread_mutex_unlock(m) ReleaseMutex(*m)
#define pthread_mutex_init(m, attr) (*m = CreateMutex(NULL, FALSE, NULL))
#define pthread_t HANDLE
#define pthread_create(thread, attr, func, arg) ((*thread = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)func, arg, 0, NULL)) == NULL)
#define pthread_join(thread, retval) WaitForSingleObject(thread, INFINITE)
#define sleep(x) Sleep((x) * 1000)
#define usleep(x) Sleep((x) / 1000)
#endif

// Function declarations
int connect_to_twitch(const char* oauth, const char* nickname, const char* channel);
void* read_chat_thread(void* arg);
void* input_thread(void* arg);
void parse_message(const char* raw_msg);
void display_terminal();
void* web_server_thread(void* arg);
void extract_badges(const char* tags, int* is_broadcaster, int* is_mod, int* is_sub, int* is_vip);
char* extract_tag_value(const char* tags, const char* key);
void save_config(const char* oauth, const char* nickname);
int load_config(char* oauth, char* nickname);
void load_blacklist();
int is_blacklisted(const char* message);
void cleanup_and_exit();
void send_chat_message(const char* message);

void init_chat_buffer() {
#ifdef _WIN32
    mutex = CreateMutex(NULL, FALSE, NULL);
#else
    pthread_mutex_init(&chat_buffer.lock, NULL);
#endif
    chat_buffer.count = 0;
}

// Load blacklist from file (because we don't want to see that bullshit)
void load_blacklist() {
    FILE* f = fopen(BLACKLIST_FILE, "r");
    if (!f) return;
    
    char line[256];
    blacklist_count = 0;
    
    while (fgets(line, sizeof(line), f) && blacklist_count < MAX_BLACKLIST_WORDS) {
        // Remove newline, because fuck newlines
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) > 0) {
            blacklist_words[blacklist_count] = strdup(line);
            blacklist_count++;
        }
    }
    
    fclose(f);
    printf("Loaded %d blacklisted words\n", blacklist_count);
}

// Check if message contains blacklisted words (filter the crap out)
int is_blacklisted(const char* message) {
    char msg_lower[MAX_MESSAGE];
    strncpy(msg_lower, message, MAX_MESSAGE - 1);
    
    // Convert to lowercase for comparison
    for (int i = 0; msg_lower[i]; i++) {
        msg_lower[i] = tolower(msg_lower[i]);
    }
    
    for (int i = 0; i < blacklist_count; i++) {
        if (strstr(msg_lower, blacklist_words[i])) {
            return 1; // Found blacklisted word, fuck this message
        }
    }
    return 0;
}

void add_message(const char* username, const char* message, int is_broadcaster, 
                 int is_mod, int is_sub, int is_vip) {
    // Don't add blacklisted messages (ain't nobody got time for that)
    if (is_blacklisted(message)) {
        return;
    }
    
    pthread_mutex_lock(&chat_buffer.lock);
    
    if (chat_buffer.count >= MAX_MESSAGES) {
        // Shift messages (out with the old shit)
        memmove(&chat_buffer.messages[0], &chat_buffer.messages[1], 
                sizeof(ChatMessage) * (MAX_MESSAGES - 1));
        chat_buffer.count = MAX_MESSAGES - 1;
    }
    
    ChatMessage* msg = &chat_buffer.messages[chat_buffer.count];
    strncpy(msg->username, username, MAX_USERNAME - 1);
    strncpy(msg->message, message, MAX_MESSAGE - 1);
    msg->is_broadcaster = is_broadcaster;
    msg->is_moderator = is_mod;
    msg->is_subscriber = is_sub;
    msg->is_vip = is_vip;
    msg->timestamp = time(NULL);
    chat_buffer.count++;
    
    pthread_mutex_unlock(&chat_buffer.lock);
}

// Save config so we don't have to enter this shit every damn time
void save_config(const char* oauth, const char* nickname) {
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) {
        perror("Couldn't save config, what the fuck");
        return;
    }
    
    fprintf(f, "%s\n%s\n", oauth, nickname);
    fclose(f);
    
    // Make it readable only by user (security and shit)
#ifndef _WIN32
    chmod(CONFIG_FILE, 0600);
#endif
}

// Load saved config (lazy mode activated)
int load_config(char* oauth, char* nickname) {
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) return 0;
    
    if (fscanf(f, "%255s\n%99s\n", oauth, nickname) != 2) {
        fclose(f);
        return 0;
    }
    
    fclose(f);
    return 1;
}

char* extract_tag_value(const char* tags, const char* key) {
    static char value[512];
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "%s=", key);
    
    char* start = strstr(tags, search_key);
    if (!start) return NULL;
    
    start += strlen(search_key);
    char* end = strchr(start, ';');
    
    int len = end ? (end - start) : strlen(start);
    if (len >= sizeof(value)) len = sizeof(value) - 1;
    
    strncpy(value, start, len);
    value[len] = '\0';
    return value;
}

void extract_badges(const char* tags, int* is_broadcaster, int* is_mod, 
                    int* is_sub, int* is_vip) {
    *is_broadcaster = 0;
    *is_mod = 0;
    *is_sub = 0;
    *is_vip = 0;
    
    char* badges = extract_tag_value(tags, "badges");
    if (!badges) return;
    
    if (strstr(badges, "broadcaster/")) *is_broadcaster = 1;
    if (strstr(badges, "moderator/")) *is_mod = 1;
    if (strstr(badges, "subscriber/")) *is_sub = 1;
    if (strstr(badges, "vip/")) *is_vip = 1;
}

void parse_message(const char* raw_msg) {
    // Handle PING like a boss
    if (strncmp(raw_msg, "PING", 4) == 0) {
        char pong[256];
        snprintf(pong, sizeof(pong), "PONG %s\r\n", raw_msg + 5);
        send(sockfd, pong, strlen(pong), 0);
        return;
    }
    
    if (strstr(raw_msg, "PRIVMSG") == NULL) return;
    
    // Extract tags (Twitch IRC is a pain in the ass)
    char* tags_end = strchr(raw_msg, ' ');
    if (!tags_end) return;
    
    char tags[1024];
    int tags_len = tags_end - raw_msg;
    if (tags_len >= sizeof(tags)) tags_len = sizeof(tags) - 1;
    strncpy(tags, raw_msg, tags_len);
    tags[tags_len] = '\0';
    
    // Extract username (who's talking this time?)
    char* user_start = strchr(tags_end + 1, ':');
    if (!user_start) return;
    user_start++;
    
    char* user_end = strchr(user_start, '!');
    if (!user_end) return;
    
    char username[MAX_USERNAME];
    int user_len = user_end - user_start;
    if (user_len >= MAX_USERNAME) user_len = MAX_USERNAME - 1;
    strncpy(username, user_start, user_len);
    username[user_len] = '\0';
    
    // Extract message (the actual content, finally)
    char* msg_start = strstr(user_end, "PRIVMSG");
    if (!msg_start) return;
    
    msg_start = strchr(msg_start, ':');
    if (!msg_start) return;
    msg_start++;
    
    char message[MAX_MESSAGE];
    strncpy(message, msg_start, MAX_MESSAGE - 1);
    message[MAX_MESSAGE - 1] = '\0';
    
    // Remove \r\n (clean this shit up)
    char* newline = strchr(message, '\r');
    if (newline) *newline = '\0';
    newline = strchr(message, '\n');
    if (newline) *newline = '\0';
    
    // Extract badges (show off those fancy icons)
    int is_broadcaster, is_mod, is_sub, is_vip;
    extract_badges(tags, &is_broadcaster, &is_mod, &is_sub, &is_vip);
    
    add_message(username, message, is_broadcaster, is_mod, is_sub, is_vip);
    
    if (display_mode == 0) {
        display_terminal();
    }
}

void* read_chat_thread(void* arg) {
    char buffer[BUFFER_SIZE];
    
    while (running) {
        int bytes = recv(sockfd, buffer, BUFFER_SIZE - 1, 0);
        if (bytes <= 0) {
            printf("Connection lost, damn it\n");
            running = 0;
            break;
        }
        
        buffer[bytes] = '\0';
        
        char* line = strtok(buffer, "\r\n");
        while (line != NULL) {
            parse_message(line);
            line = strtok(NULL, "\r\n");
        }
    }
    
    return NULL;
}

// Send a message to chat (if you wanna talk back)
void send_chat_message(const char* message) {
    char send_buffer[BUFFER_SIZE];
    snprintf(send_buffer, sizeof(send_buffer), "PRIVMSG #%s :%s\r\n", target_channel, message);
    send(sockfd, send_buffer, strlen(send_buffer), 0);
}

// Input thread for terminal mode (so you can type shit)
void* input_thread(void* arg) {
    char input[MAX_MESSAGE];
    
    while (running && display_mode == 0) {
        // Position cursor at bottom
        printf("\033[%d;0H", 30); // Line 30
        printf("\033[K"); // Clear line
        printf("Type message (or 'q' to quit): ");
        fflush(stdout);
        
        if (fgets(input, sizeof(input), stdin) == NULL) {
            break;
        }
        
        // Remove newline
        input[strcspn(input, "\n")] = 0;
        
        // Check for quit command
        if (strcmp(input, "q") == 0 || strcmp(input, "Q") == 0) {
            running = 0;
            break;
        }
        
        // Send message if not empty
        if (strlen(input) > 0) {
            send_chat_message(input);
        }
        
        display_terminal();
    }
    
    return NULL;
}

int connect_to_twitch(const char* oauth, const char* nickname, const char* channel) {
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        printf("WSAStartup failed\n");
        return -1;
    }
#endif

    struct hostent* server;
    struct sockaddr_in serv_addr;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
#ifdef _WIN32
    if (sockfd == INVALID_SOCKET) {
#else
    if (sockfd < 0) {
#endif
        perror("Error opening socket, fuck");
        return -1;
    }
    
    server = gethostbyname(IRC_SERVER);
    if (server == NULL) {
        fprintf(stderr, "Error: No such host, what the hell\n");
        return -1;
    }
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(IRC_PORT);
    
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error connecting, shit");
        return -1;
    }
    
    // Send authentication (let me in!)
    char auth[512];
    snprintf(auth, sizeof(auth), "PASS oauth:%s\r\n", oauth);
    send(sockfd, auth, strlen(auth), 0);
    
    snprintf(auth, sizeof(auth), "NICK %s\r\n", nickname);
    send(sockfd, auth, strlen(auth), 0);
    
    // Request capabilities for tags (we want the fancy badges)
    char* caps = "CAP REQ :twitch.tv/tags twitch.tv/commands\r\n";
    send(sockfd, caps, strlen(caps), 0);
    
    // Join channel (let's fucking goooo)
    snprintf(auth, sizeof(auth), "JOIN #%s\r\n", channel);
    send(sockfd, auth, strlen(auth), 0);
    
    strncpy(target_channel, channel, MAX_USERNAME - 1);
    
    return 0;
}

void display_terminal() {
#ifdef _WIN32
    system("cls");
#else
    system("clear");
#endif
    printf("=== Twitch Chat: #%s ===\n", target_channel);
    printf("(Press 'q' and Enter to quit)\n\n");
    
    pthread_mutex_lock(&chat_buffer.lock);
    
    for (int i = 0; i < chat_buffer.count; i++) {
        ChatMessage* msg = &chat_buffer.messages[i];
        
        printf("\033[1;34m%s\033[0m", msg->username);
        
        if (msg->is_broadcaster) printf(" \033[1;31m[BROADCASTER]\033[0m");
        if (msg->is_moderator) printf(" \033[1;32m[MOD]\033[0m");
        if (msg->is_subscriber) printf(" \033[1;35m[SUB]\033[0m");
        if (msg->is_vip) printf(" \033[1;33m[VIP]\033[0m");
        
        printf(": \033[0;36m%s\033[0m\n", msg->message);
    }
    
    pthread_mutex_unlock(&chat_buffer.lock);
    printf("\n");
}

void* web_server_thread(void* arg) {
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);
    
    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(5000);
    
    bind(server_fd, (struct sockaddr*)&address, sizeof(address));
    listen(server_fd, 3);
    
    printf("Web server running on http://localhost:5000\n");
    printf("Add this URL to OBS Browser Source\n");
    printf("Press 'q' and Enter to quit\n\n");
    
    while (running) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(server_fd, &readfds);
        
        int activity = select(server_fd + 1, &readfds, NULL, NULL, &tv);
        if (activity <= 0) continue;
        
        new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) continue;
        
        char request[1024];
        recv(new_socket, request, sizeof(request), 0);
        
        // Check if requesting CSS file
        if (strstr(request, "GET /twitchchat.css")) {
            FILE* css_file = fopen(CSS_FILE, "r");
            if (css_file) {
                char css_response[8192];
                char css_content[7000];
                size_t css_size = fread(css_content, 1, sizeof(css_content) - 1, css_file);
                css_content[css_size] = '\0';
                fclose(css_file);
                
                snprintf(css_response, sizeof(css_response),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/css\r\n\r\n%s", css_content);
                send(new_socket, css_response, strlen(css_response), 0);
            }
        } else {
            // Serve HTML with messages
            char response[8192];
            
            pthread_mutex_lock(&chat_buffer.lock);
            
            char messages_html[6144] = "";
            for (int i = 0; i < chat_buffer.count; i++) {
                ChatMessage* msg = &chat_buffer.messages[i];
                char badges[256] = "";
                
                if (msg->is_broadcaster) strcat(badges, "<span class=\"badge broadcaster\">B</span>");
                if (msg->is_moderator) strcat(badges, "<span class=\"badge moderator\">MOD</span>");
                if (msg->is_subscriber) strcat(badges, "<span class=\"badge subscriber\">SUB</span>");
                if (msg->is_vip) strcat(badges, "<span class=\"badge vip\">VIP</span>");
                
                char msg_line[1024];
                snprintf(msg_line, sizeof(msg_line),
                    "<div class=\"message\"><span class=\"username\">%s</span>%s: <span class=\"text\">%s</span></div>\n",
                    msg->username, badges, msg->message);
                strcat(messages_html, msg_line);
            }
            
            pthread_mutex_unlock(&chat_buffer.lock);
            
            // Read HTML template
            FILE* html_file = fopen(HTML_FILE, "r");
            char html_template[2048];
            if (html_file) {
                size_t html_size = fread(html_template, 1, sizeof(html_template) - 1, html_file);
                html_template[html_size] = '\0';
                fclose(html_file);
                
                // Insert messages into HTML
                char* container_pos = strstr(html_template, "<!-- Messages will be inserted here -->");
                if (container_pos) {
                    *container_pos = '\0';
                    snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n%s%s</div></body></html>",
                        html_template, messages_html);
                } else {
                    snprintf(response, sizeof(response),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n<html><body>%s</body></html>",
                        messages_html);
                }
            } else {
                // Fallback if HTML file not found
                snprintf(response, sizeof(response),
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n"
                    "<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"2\">"
                    "<link rel=\"stylesheet\" href=\"twitchchat.css\"></head>"
                    "<body><div class=\"chat-container\">%s</div></body></html>",
                    messages_html);
            }
            
            send(new_socket, response, strlen(response), 0);
        }
        
        close(new_socket);
    }
    
    close(server_fd);
    return NULL;
}

// Cleanup and properly disconnect (don't be a dick, say goodbye)
void cleanup_and_exit() {
    printf("\n\nDisconnecting from Twitch IRC...\n");
    
#ifdef _WIN32
    if (sockfd != INVALID_SOCKET) {
#else
    if (sockfd >= 0) {
#endif
        // Send PART command to leave channel
        char part_msg[256];
        snprintf(part_msg, sizeof(part_msg), "PART #%s\r\n", target_channel);
        send(sockfd, part_msg, strlen(part_msg), 0);
        
        // Send QUIT command
        char* quit_msg = "QUIT :Goodbye!\r\n";
        send(sockfd, quit_msg, strlen(quit_msg), 0);
        
        usleep(500000); // Wait 500ms for messages to send
        close(sockfd);
    }
    
#ifdef _WIN32
    WSACleanup();
#endif
    
    // Free blacklist words (don't leak memory like a scrub)
    for (int i = 0; i < blacklist_count; i++) {
        free(blacklist_words[i]);
    }
    
    printf("Disconnected. See ya!\n");
}

void print_help() {
    printf("Twitch Chat Reader - C Edition\n");
    printf("================================\n\n");
    printf("This program connects to Twitch IRC and displays chat messages.\n\n");
    printf("To get an OAuth token (choose one method):\n\n");
    printf("METHOD 1 - Quick & Easy (Recommended):\n");
    printf("  1. Visit: https://twitchtokengenerator.com/\n");
    printf("  2. Click 'Connect' and authorize\n");
    printf("  3. Copy the access token (without 'oauth:' prefix)\n\n");
    printf("METHOD 2 - Alternative:\n");
    printf("  1. Visit: https://antiscuff.com/oauth/\n");
    printf("  2. Follow the authorization steps\n");
    printf("  3. Copy the OAuth token (without 'oauth:' prefix)\n\n");
    printf("Usage:\n");
    printf("  ./twitchchat              # Use saved credentials\n");
    printf("  ./twitchchat --new-token  # Enter new token\n");
    printf("  ./twitchchat --help       # Show this help\n\n");
    printf("Features:\n");
    printf("  - Auto-saves OAuth token for reuse\n");
    printf("  - Terminal mode: Type messages, press 'q' to quit\n");
    printf("  - Web mode: OBS-ready chat overlay\n");
    printf("  - Blacklist support: Create 'blacklist.txt' to filter words\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        print_help();
        return 0;
    }
    
    char oauth[256], nickname[MAX_USERNAME], channel[MAX_USERNAME];
    int mode;
    int use_saved = 0;
    
    // Check if we should use saved token
    if (argc == 1 || (argc > 1 && strcmp(argv[1], "--new-token") != 0)) {
        use_saved = load_config(oauth, nickname);
    }
    
    printf("=== Twitch Chat Reader ===\n\n");
    
    if (use_saved && argc == 1) {
        printf("Using saved credentials for: %s\n", nickname);
        printf("(Use --new-token to override)\n\n");
    } else {
        printf("Get your OAuth token from:\n");
        printf("  - https://twitchtokengenerator.com/ (Recommended)\n");
        printf("  - https://antiscuff.com/oauth/\n");
        printf("(Enter token without 'oauth:' prefix)\n\n");
        
        printf("OAuth Token: ");
        if (scanf("%255s", oauth) != 1) {
            printf("Invalid input\n");
            return 1;
        }
        
        printf("Your Twitch Username: ");
        if (scanf("%99s", nickname) != 1) {
            printf("Invalid input\n");
            return 1;
        }
        
        save_config(oauth, nickname);
        printf("Credentials saved!\n\n");
    }
    
    strncpy(my_nickname, nickname, MAX_USERNAME - 1);
    
    printf("Channel to Monitor: ");
    if (scanf("%99s", channel) != 1) {
        printf("Invalid input\n");
        return 1;
    }
    
    printf("\nDisplay Mode:\n");
    printf("1. Terminal Display (interactive)\n");
    printf("2. Web Chatbox (for OBS)\n");
    printf("Choice: ");
    if (scanf("%d", &mode) != 1 || (mode != 1 && mode != 2)) {
        printf("Invalid choice\n");
        return 1;
    }
    
    display_mode = (mode == 2) ? 1 : 0;
    
    // Load blacklist (filter out the garbage)
    load_blacklist();
    
    init_chat_buffer();
    
    printf("\nConnecting to Twitch IRC...\n");
    if (connect_to_twitch(oauth, nickname, channel) < 0) {
        printf("Failed to connect\n");
        return 1;
    }
    
    printf("Connected! Monitoring #%s\n\n", channel);
    
    // Set up signal handler for cleanup
    signal(SIGINT, cleanup_and_exit);
    
    pthread_t chat_thread;
    pthread_create(&chat_thread, NULL, read_chat_thread, NULL);
    
    if (display_mode == 1) {
        // Web server mode
        pthread_t web_thread;
        pthread_create(&web_thread, NULL, web_server_thread, NULL);
        
        // Simple input check for quit
        char input[10];
        while (running) {
            if (fgets(input, sizeof(input), stdin)) {
                if (input[0] == 'q' || input[0] == 'Q') {
                    running = 0;
                    break;
                }
            }
        }
        
        pthread_join(web_thread, NULL);
    } else {
        // Terminal mode with input
        pthread_t input_t;
        pthread_create(&input_t, NULL, input_thread, NULL);
        
        while (running) {
            sleep(1);
        }
        
        pthread_join(input_t, NULL);
    }
    
    pthread_join(chat_thread, NULL);
    
    cleanup_and_exit();
    
    return 0;
}
