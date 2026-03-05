#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <sys/timerfd.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#define PORT "8080"
#define MAX_EVENTS 1000
#define BUF_SIZE 1000
#define BACKLOG 100
#define MAX_ALLOWED 8000
#define EVENT_LISTENER 0
#define EVENT_PLAYER 1
#define EVENT_TIMER  2

typedef struct Player Player;

typedef struct Game {
    Player *player1;
    Player *player2;
    char board[9];
    char current_turn;
    int game_over;
    struct Game *next;   
}Game;

typedef struct Player {
    int type;
    int player_fd; 
    char symbol;
    Game *game;
    int enqueued;
    int handshake_done; 
}Player;

typedef struct WaitingPlayer {
    Player *player;
    struct WaitingPlayer *next;
} WaitingPlayer;

typedef struct TimerEvent {
    int type;
    int timer_fd;
    Player *player;
} TimerEvent;

typedef struct Listener {
    int type;
    int sockfd;
} Listener;

static Game *game_head = NULL;
static WaitingPlayer *queue_head = NULL;
static WaitingPlayer *queue_tail = NULL;

void send_text_frame(int client_fd, const char *msg){
    
    unsigned char frame[BUF_SIZE];
    size_t len = strlen(msg);
    frame[0] = 0x81; // 1000 0001
    if (len <= 125) {
        frame[1] = len;
        memcpy(&frame[2], msg, len);
        send(client_fd, frame, len + 2, 0);
    }
    else if (len <= 65535) {
        frame[1] = 126;
        frame[2] = (len >> 8) & 0xFF; //MSB
        frame[3] = len & 0xFF;  //LSB
        memcpy(&frame[4], msg, len);
        send(client_fd, frame, len + 4, 0);
    }
}

void broadcast_board(Game *game){

    char msg[512];
    snprintf(msg, sizeof(msg), "{\"type\":\"update\",\"board\":[\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\"]}", game->board[0], game->board[1], game->board[2], game->board[3], game->board[4], game->board[5], game->board[6], game->board[7], game->board[8]);
    if (game->player1 && game->player1->player_fd)
        send_text_frame(game->player1->player_fd, msg);
    if (game->player2 && game->player2->player_fd)
        send_text_frame(game->player2->player_fd, msg);
}

char check_winner(Game *game){

    int wins[8][3] = {
        {0,1,2},{3,4,5},{6,7,8},
        {0,3,6},{1,4,7},{2,5,8},
        {0,4,8},{2,4,6}
    };
    for (int i = 0; i < 8; i++)
    {
        int a = wins[i][0];
        int b = wins[i][1];
        int c = wins[i][2];
        if (game->board[a] != ' ' && game->board[a] == game->board[b] && game->board[a] == game->board[c])
        {
            return game->board[a];
        }
    }
    return ' ';
}

int is_draw(Game *game){

    for (int i = 0; i < 9; i++)
    {
        if (game->board[i] == ' ')
            return 0;
    }
    return 1;
}

void remove_game(Game *game){

    Game **curr = &game_head;
    while (*curr)
    {
        if (*curr == game)
        {
            *curr = game->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

Player *dequeue_player(){

    if (!queue_head)
        return NULL;
    WaitingPlayer *node = queue_head;
    Player *p = node->player;
    queue_head = node->next;
    if (!queue_head)
        queue_tail = NULL;
    free(node);
    p->enqueued = 0;
    return p;
}

void match_players(){

    while (queue_head && queue_head->next)
    {
        Player *p1 = dequeue_player();
        Player *p2 = dequeue_player();
        Game *game = malloc(sizeof(Game));
        memset(game, 0, sizeof(Game));
        for (int i = 0; i < 9; i++)
            game->board[i] = ' ';
        game->player1 = p1;
        game->player2 = p2;
        game->current_turn = 'X';
        p1->symbol = 'X';
        p2->symbol = 'O';
        p1->game = game;
        p2->game = game;
        game->next = game_head;
        game_head = game;
        send_text_frame(p1->player_fd, "{\"type\":\"assign\",\"symbol\":\"You are player X !\"}");
        send_text_frame(p2->player_fd, "{\"type\":\"assign\",\"symbol\":\"You are player O !\"}");
        broadcast_board(game);
    }
}

void enqueue_player(Player *p){

    if (p->enqueued) 
        return; 
    WaitingPlayer *node = malloc(sizeof(WaitingPlayer));
    node->player = p;
    node->next = NULL;
    if (!queue_tail)
        queue_head = queue_tail = node;
    else
    {
        queue_tail->next = node;
        queue_tail = node;
    }
    p->enqueued = 1;
}

void remove_from_queue(Player *p){

    WaitingPlayer **curr = &queue_head;
    WaitingPlayer *prev = NULL;
    while (*curr)
    {
        WaitingPlayer *node = *curr;
        if (node->player == p)
        {
            *curr = node->next;
            if (node == queue_tail)
                queue_tail = prev;
            p->enqueued = 0;
            free(node);
            return;
        }
        prev = node;
        curr = &node->next;
    }
}

int make_socket_non_blocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void generate_websocket_accept_key(const char *client_key, char *accept_key)
{
    const char *magic_string = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    char combined[256];
    unsigned char sha1[SHA_DIGEST_LENGTH];
    snprintf(combined, sizeof(combined), "%s%s", client_key, magic_string);
    SHA1((unsigned char *)combined, strlen(combined), sha1);
    EVP_EncodeBlock((unsigned char *)accept_key, sha1, SHA_DIGEST_LENGTH);
}

void accept_connections(int sockfd, int epfd){
    struct epoll_event ev;
    while (1) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof client_addr;
        int client_fd = accept(sockfd, (struct sockaddr *)&client_addr, &addr_len);
        if (client_fd == -1) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            else {
                perror("accept");
                break;
            }
        }
        make_socket_non_blocking(client_fd);
        printf("New client connected: %d\n\n", client_fd);
        Player *p = malloc(sizeof(Player));
        memset(p, 0, sizeof(Player));
        p->type = EVENT_PLAYER;
        p->player_fd = client_fd;
        p->game = NULL;
        p->symbol = ' ';
        p->enqueued = 0;
        p->handshake_done = 0;
        ev.events = EPOLLIN;
        ev.data.ptr = p;
        epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);
    }
}

void handle_disconnect(Player *p, int epfd){
    
    if (!p)
        return;
    if (p->game) {
        Game *g = p->game;
        Player *opponent = (g->player1 == p) ? g->player2 : g->player1;
        if (opponent) {
            send_text_frame(opponent->player_fd, 
                "{\"type\":\"opponent_left\"}");
            opponent->game = NULL;
            opponent->symbol = ' ';
            opponent->enqueued = 0;
            int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
            if (tfd == -1) {
                perror("timerfd_create");
                return;
            }
            struct itimerspec ts;
            memset(&ts, 0, sizeof(ts));
            ts.it_value.tv_sec = 2;   
            timerfd_settime(tfd, 0, &ts, NULL);
            TimerEvent *te = malloc(sizeof(TimerEvent));
            te->type = EVENT_TIMER;
            te->timer_fd = tfd;
            te->player = opponent;
            struct epoll_event ev;
            ev.events = EPOLLIN;
            ev.data.ptr = te;
            epoll_ctl(epfd, EPOLL_CTL_ADD, tfd, &ev);
        }
        remove_game(g);
        free(g);
        match_players();
    }
    remove_from_queue(p);
    epoll_ctl(epfd, EPOLL_CTL_DEL, p->player_fd, NULL);
    close(p->player_fd);
    p->handshake_done = 0;
    free(p);
}

void handle_handshake(Player *p, unsigned char *buf){

    if (strstr((char *)buf,"Connection: Upgrade") && strstr((char *)buf,"Upgrade: websocket")){
        char *key_ptr = strstr((char *)buf,"Sec-WebSocket-Key:");
        if (!key_ptr) return;
        key_ptr += 19;
        printf("HANDSHAKE REQUEST \n");
        char client_key[256] = {0};
        sscanf(key_ptr, "%255[^\r\n]", client_key);
        char accept_key[128];
        generate_websocket_accept_key(client_key, accept_key);
        char resp[512];
        snprintf(resp, sizeof(resp),
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: %s\r\n"
            "\r\n",
            accept_key);
        send(p->player_fd, resp, strlen(resp), 0);
        printf("Handshake completed: %d\n\n", p->player_fd);
        p->handshake_done = 1;
        enqueue_player(p);
        char msg[] = "{\"type\":\"connected\",\"symbol\":\"Waiting for opponent player ...\"}";
        send_text_frame(p->player_fd, msg);
        match_players(); 
        return;
    }
}

void handle_game_message(Player *p, char *buf, int mask_offset, unsigned long payload_len){
    unsigned char masking_key[4];
    memcpy(masking_key, &buf[mask_offset], 4);
    unsigned char *payload = &buf[mask_offset + 4];
    char decoded[BUF_SIZE];
    for (unsigned long j = 0; j < payload_len; j++) {
        decoded[j] = payload[j] ^ masking_key[j % 4];
    }
    decoded[payload_len] = '\0';
    char *msg = strndup((char*)decoded, payload_len);
    if (!msg)
        return;
    Game *game = p->game;
    if (!game){
        free(msg);
        return;
    }
    if (strstr(msg, "\"type\":\"move\"")){
        int pos = -1;
        sscanf(msg, "{\"type\":\"move\",\"position\":%d}", &pos);
        if (!game->game_over && pos >= 0 && pos < 9 && game->board[pos] == ' ' && p->symbol == game->current_turn){
            game->board[pos] = p->symbol;
            broadcast_board(game);
            char winner = check_winner(game);
            if (winner != ' '){
                game->game_over = 1;
                char result_msg[128];
                snprintf(result_msg, sizeof(result_msg), "{\"type\":\"result\",\"message\":\"Winner is %c\"}", winner);
                send_text_frame(game->player1->player_fd, result_msg);
                send_text_frame(game->player2->player_fd, result_msg);
            }
            else if (is_draw(game)){
                game->game_over = 1;
                char draw_msg[] = "{\"type\":\"result\",\"message\":\"Match Draw\"}";
                send_text_frame(game->player1->player_fd, draw_msg);
                send_text_frame(game->player2->player_fd, draw_msg);
            }
            game->current_turn = (game->current_turn == 'X') ? 'O' : 'X';
        }
    }
    else if (strstr(msg, "\"type\":\"reset\"")){
        for (int i = 0; i < 9; i++)
            game->board[i] = ' ';
        game->current_turn = 'X';
        game->game_over = 0;
        broadcast_board(game);
        char reset_msg[] = "{\"type\":\"reset\"}";
        send_text_frame(game->player1->player_fd, reset_msg);
        send_text_frame(game->player2->player_fd, reset_msg);
    }
    free(msg);
    return;
}

void handle_websocket_frame(Player *p, unsigned char *buf, int n, int epfd){
    unsigned char byte1 = buf[0];
    unsigned char byte2 = buf[1];
    int opcode = byte1 & 0x0F;
    int mask = (byte2 >> 7) & 1;
    unsigned long payload_len = byte2 & 0x7F;
    int mask_offset;
    if (payload_len == 126) {
        payload_len = (buf[2] << 8) | buf[3];
        mask_offset = 4;
    }
    else if (payload_len == 127) {
        return;
    }
    else {
        mask_offset = 2;
    }
    if (opcode == 0x8 && mask == 1) {
        handle_disconnect(p, epfd);
        return;
    }
    else if(opcode == 0x1 && mask == 1){
        handle_game_message(p, buf, mask_offset, payload_len);
        return;
    }
}

void handle_client_event(Player *p, int epfd)
{
    unsigned char buf[BUF_SIZE];
    int n = recv(p->player_fd, buf, BUF_SIZE, 0);
    if (n <= 0) {
        handle_disconnect(p, epfd);
        return;
    }
    if (!p->handshake_done) {
        handle_handshake(p, buf);
    } else {
        handle_websocket_frame(p, buf, n, epfd);
    }
}

int main(void)
{
    int sockfd, epfd, rv, yes = 1;
    struct addrinfo hints, *res, *p;
    struct epoll_event ev, events[MAX_EVENTS];
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    if ((rv = getaddrinfo(NULL, PORT, &hints, &res)) != 0) {
        fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
        return 1;
    }

    for (p = res; p != NULL; p = p->ai_next) {
        sockfd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (sockfd == -1) continue;
        setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));
        if (bind(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
            close(sockfd);
            continue;
        }
        break;
    }

    if (p == NULL) {
        printf("Failed to bind\n");
        return 1;
    }

    listen(sockfd, BACKLOG);
    make_socket_non_blocking(sockfd);

    epfd = epoll_create1(0);
    Listener *listener = malloc(sizeof(Listener));
    listener->type = EVENT_LISTENER;
    listener->sockfd = sockfd;
    ev.events = EPOLLIN;
    ev.data.ptr = listener;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);            

    printf("Tic Tac Toe server running on port %s\n", PORT);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {

            void *ptr = events[i].data.ptr;
            int type = *((int*)ptr);
            if (type == EVENT_LISTENER) {
                Listener *l = ptr;
                accept_connections(l->sockfd, epfd);
            }
            else if (type == EVENT_PLAYER) {
                Player *p = ptr;
                handle_client_event(p, epfd);
            }
            else if (type == EVENT_TIMER) {
                TimerEvent *te = ptr;
                uint64_t exp;
                read(te->timer_fd, &exp, sizeof(exp));
                enqueue_player(te->player);
                match_players();
                epoll_ctl(epfd, EPOLL_CTL_DEL, te->timer_fd, NULL);
                close(te->timer_fd);
                free(te);
             }
        }
    }
    close(sockfd);
    freeaddrinfo(res);
    return 0;
}
