#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <pthread.h>
#include <time.h>
#include <arpa/inet.h>

#define IRC_SERVER "irc.chat.twitch.tv"
#define IRC_PORT 6667
#define BUFFER_SIZE 4096
#define MAX_MESSAGES 50
#define MAX_USERNAME 100
#define MAX_MESSAGE 512

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

// Global variables
int sockfd = -1;
ChatBuffer chat_buffer = {.count = 0};
int running = 1;
int display_mode = 0; // 0 = terminal, 1 = web server
char target_channel[MAX_USERNAME];

// Function declarations
int connect_to_twitch(const char* oauth, const char* nickname, const char* channel);
void* read_chat_thread(void* arg);
void parse_message(const char* raw_msg);
void display_terminal();
void* web_server_thread(void* arg);
void extract_badges(const char* tags, int* is_broadcaster, int* is_mod, int* is_sub, int* is_vip);
char* extract_tag_value(const char* tags, const char* key);

void init_chat_buffer() {
    pthread_mutex_init(&chat_buffer.lock, NULL);
    chat_buffer.count = 0;
}

void add_message(const char* username, const char* message, int is_broadcaster, 
                 int is_mod, int is_sub, int is_vip) {
    pthread_mutex_lock(&chat_buffer.lock);
    
    if (chat_buffer.count >= MAX_MESSAGES) {
        // Shift messages
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
    if (strncmp(raw_msg, "PING", 4) == 0) {
        char pong[256];
        snprintf(pong, sizeof(pong), "PONG %s\r\n", raw_msg + 5);
        send(sockfd, pong, strlen(pong), 0);
        return;
    }
    
    if (strstr(raw_msg, "PRIVMSG") == NULL) return;
    
    // Extract tags
    char* tags_end = strchr(raw_msg, ' ');
    if (!tags_end) return;
    
    char tags[1024];
    int tags_len = tags_end - raw_msg;
    if (tags_len >= sizeof(tags)) tags_len = sizeof(tags) - 1;
    strncpy(tags, raw_msg, tags_len);
    tags[tags_len] = '\0';
    
    // Extract username
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
    
    // Extract message
    char* msg_start = strstr(user_end, "PRIVMSG");
    if (!msg_start) return;
    
    msg_start = strchr(msg_start, ':');
    if (!msg_start) return;
    msg_start++;
    
    char message[MAX_MESSAGE];
    strncpy(message, msg_start, MAX_MESSAGE - 1);
    message[MAX_MESSAGE - 1] = '\0';
    
    // Remove \r\n
    char* newline = strchr(message, '\r');
    if (newline) *newline = '\0';
    newline = strchr(message, '\n');
    if (newline) *newline = '\0';
    
    // Extract badges
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
            printf("Connection lost\n");
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

int connect_to_twitch(const char* oauth, const char* nickname, const char* channel) {
    struct hostent* server;
    struct sockaddr_in serv_addr;
    
    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        perror("Error opening socket");
        return -1;
    }
    
    server = gethostbyname(IRC_SERVER);
    if (server == NULL) {
        fprintf(stderr, "Error: No such host\n");
        return -1;
    }
    
    memset(&serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    memcpy(&serv_addr.sin_addr.s_addr, server->h_addr, server->h_length);
    serv_addr.sin_port = htons(IRC_PORT);
    
    if (connect(sockfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("Error connecting");
        return -1;
    }
    
    // Send authentication
    char auth[512];
    snprintf(auth, sizeof(auth), "PASS oauth:%s\r\n", oauth);
    send(sockfd, auth, strlen(auth), 0);
    
    snprintf(auth, sizeof(auth), "NICK %s\r\n", nickname);
    send(sockfd, auth, strlen(auth), 0);
    
    // Request capabilities for tags (badges)
    char* caps = "CAP REQ :twitch.tv/tags twitch.tv/commands\r\n";
    send(sockfd, caps, strlen(caps), 0);
    
    // Join channel
    snprintf(auth, sizeof(auth), "JOIN #%s\r\n", channel);
    send(sockfd, auth, strlen(auth), 0);
    
    strncpy(target_channel, channel, MAX_USERNAME - 1);
    
    return 0;
}

void display_terminal() {
    system("clear");
    printf("=== Twitch Chat: #%s ===\n\n", target_channel);
    
    pthread_mutex_lock(&chat_buffer.lock);
    
    for (int i = 0; i < chat_buffer.count; i++) {
        ChatMessage* msg = &chat_buffer.messages[i];
        
        printf("\033[1;34m%s\033[0m", msg->username);
        
        if (msg->is_broadcaster) printf(" \033[1;31m(B)\033[0m");
        if (msg->is_moderator) printf(" \033[1;32m(MOD)\033[0m");
        if (msg->is_subscriber) printf(" \033[1;35m(SUB)\033[0m");
        if (msg->is_vip) printf(" \033[1;33m(VIP)\033[0m");
        
        printf(": \033[0;36m%s\033[0m\n", msg->message);
    }
    
    pthread_mutex_unlock(&chat_buffer.lock);
    
    printf("\n[Press Ctrl+C to exit]\n");
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
    printf("Add this URL to OBS Browser Source\n\n");
    
    while (running) {
        new_socket = accept(server_fd, (struct sockaddr*)&address, (socklen_t*)&addrlen);
        
        char response[8192];
        char html_header[] = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n\r\n";
        
        pthread_mutex_lock(&chat_buffer.lock);
        
        char messages_html[6144] = "";
        for (int i = 0; i < chat_buffer.count; i++) {
            ChatMessage* msg = &chat_buffer.messages[i];
            char badges[256] = "";
            
            if (msg->is_broadcaster) strcat(badges, " <span class=\"badge broadcaster\">(B)</span>");
            if (msg->is_moderator) strcat(badges, " <span class=\"badge moderator\">(MOD)</span>");
            if (msg->is_subscriber) strcat(badges, " <span class=\"badge subscriber\">(SUB)</span>");
            if (msg->is_vip) strcat(badges, " <span class=\"badge vip\">(VIP)</span>");
            
            char msg_line[1024];
            snprintf(msg_line, sizeof(msg_line),
                "<div class=\"message\"><span class=\"username\">%s</span>%s: <span class=\"text\">%s</span></div>\n",
                msg->username, badges, msg->message);
            strcat(messages_html, msg_line);
        }
        
        pthread_mutex_unlock(&chat_buffer.lock);
        
        snprintf(response, sizeof(response),
            "%s<!DOCTYPE html><html><head><meta http-equiv=\"refresh\" content=\"2\">"
            "<style>"
            "body{font-family:Arial,sans-serif;margin:0;padding:20px;background:transparent;color:white;font-size:18px;}"
            ".chat-container{max-width:600px;max-height:500px;overflow-y:auto;background:rgba(0,0,0,0.3);border-radius:10px;padding:15px;}"
            ".message{margin-bottom:8px;word-wrap:break-word;}"
            ".username{color:#4A90E2;font-weight:bold;}"
            ".text{color:#00FFFF;margin-left:5px;}"
            ".badge{font-weight:bold;font-size:14px;}"
            ".badge.broadcaster{color:#E91916;}"
            ".badge.moderator{color:#00AD03;}"
            ".badge.subscriber{color:#6441A4;}"
            ".badge.vip{color:#E005B9;}"
            "</style></head><body>"
            "<div class=\"chat-container\">%s</div>"
            "</body></html>",
            html_header, messages_html);
        
        send(new_socket, response, strlen(response), 0);
        close(new_socket);
    }
    
    close(server_fd);
    return NULL;
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
    printf("METHOD 3 - Official Twitch (Advanced):\n");
    printf("  1. Visit: https://dev.twitch.tv/docs/authentication/\n");
    printf("  2. Register an application\n");
    printf("  3. Use OAuth authorization flow\n\n");
    printf("Usage: ./twitch_chat\n");
}

int main(int argc, char* argv[]) {
    if (argc > 1 && strcmp(argv[1], "--help") == 0) {
        print_help();
        return 0;
    }
    
    char oauth[256], nickname[MAX_USERNAME], channel[MAX_USERNAME];
    int mode;
    
    printf("=== Twitch Chat Reader ===\n\n");
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
    
    printf("Channel to Monitor: ");
    if (scanf("%99s", channel) != 1) {
        printf("Invalid input\n");
        return 1;
    }
    
    printf("\nDisplay Mode:\n");
    printf("1. Terminal Display\n");
    printf("2. Web Chatbox (for OBS)\n");
    printf("Choice: ");
    if (scanf("%d", &mode) != 1 || (mode != 1 && mode != 2)) {
        printf("Invalid choice\n");
        return 1;
    }
    
    display_mode = (mode == 2) ? 1 : 0;
    
    init_chat_buffer();
    
    printf("\nConnecting to Twitch IRC...\n");
    if (connect_to_twitch(oauth, nickname, channel) < 0) {
        printf("Failed to connect\n");
        return 1;
    }
    
    printf("Connected! Monitoring #%s\n\n", channel);
    
    pthread_t chat_thread;
    pthread_create(&chat_thread, NULL, read_chat_thread, NULL);
    
    if (display_mode == 1) {
        pthread_t web_thread;
        pthread_create(&web_thread, NULL, web_server_thread, NULL);
        pthread_join(web_thread, NULL);
    } else {
        while (running) {
            sleep(1);
        }
    }
    
    pthread_join(chat_thread, NULL);
    
    if (sockfd >= 0) {
        close(sockfd);
    }
    
    printf("Disconnected.\n");
    return 0;
}
