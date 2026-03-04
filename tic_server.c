#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <netdb.h>
#include <openssl/sha.h>
#include <openssl/evp.h>

#define PORT "8080"
#define MAX_EVENTS 1000
#define BUF_SIZE 1000
#define BACKLOG 100
#define MAX_ALLOWED 8000

int handshake_done[10000] = {0};
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
    int player_fd; 
    char symbol;
    Game *game;
    int enqueued; 
}Player;

typedef struct WaitingPlayer {
    Player *player;
    struct WaitingPlayer *next;
} WaitingPlayer;

static Game *game_head = NULL;
static WaitingPlayer *queue_head = NULL;
static WaitingPlayer *queue_tail = NULL;
Player *players[10000] = {0};

void send_text_frame(int client_fd, const char *msg)
{

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


void broadcast_board(Game *game)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "{\"type\":\"update\",\"board\":[\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\"]}", game->board[0], game->board[1], game->board[2], game->board[3], game->board[4], game->board[5], game->board[6], game->board[7], game->board[8]);
    if (game->player1 && game->player1->player_fd)
        send_text_frame(game->player1->player_fd, msg);
    if (game->player2 && game->player2->player_fd)
        send_text_frame(game->player2->player_fd, msg);
}

char check_winner(Game *game)
{
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

int is_draw(Game *game)
{
    for (int i = 0; i < 9; i++)
    {
        if (game->board[i] == ' ')
            return 0;
    }
    return 1;
}

void remove_game(Game *game)
{
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

Player *dequeue_player()
{
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

void match_players()
{
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

void enqueue_player(Player *p)
{
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

void remove_from_queue(Player *p)
{
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

    ev.events = EPOLLIN;
    ev.data.fd = sockfd;
    epoll_ctl(epfd, EPOLL_CTL_ADD, sockfd, &ev);

    printf("Tic Tac Toe server running on port %s\n", PORT);

    while (1) {
        int nfds = epoll_wait(epfd, events, MAX_EVENTS, -1);

        for (int i = 0; i < nfds; i++) {

            if (events[i].data.fd == sockfd) {

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

                    ev.events = EPOLLIN;
                    ev.data.fd = client_fd;
                    epoll_ctl(epfd, EPOLL_CTL_ADD, client_fd, &ev);

                    handshake_done[client_fd] = 0;

                    printf("New client connected: %d\n\n", client_fd);

                    Player *p = malloc(sizeof(Player));
                    memset(p, 0, sizeof(Player));
                    
                    players[client_fd] = p;
                    p->player_fd = client_fd;
                    p->game = NULL;
                    p->symbol = ' ';
                    p->enqueued = 0;
                }
            }
            else {

                int client_fd = events[i].data.fd;
                unsigned char buf[BUF_SIZE];

                int n = recv(client_fd, buf, BUF_SIZE, 0);

                if (n <= 0) {
                    Player *p = players[client_fd];
                    if (p) {
                        if (p->game) {
                            Game *g = p->game;
                            Player *opponent = (g->player1 == p) ? g->player2 : g->player1;
                            if (opponent) {
                                send_text_frame(opponent->player_fd, "{\"type\":\"opponent_left\"}");
                                opponent->game = NULL;
                                opponent->symbol = ' ';
                            }
                            remove_game(g);
                            free(g);
                        }
                        remove_from_queue(p);
                        free(p);
                        players[client_fd] = NULL;
                    }
                    epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, NULL);
                    handshake_done[client_fd] = 0;
                    close(client_fd);
                    continue;
                }
                
                if (!handshake_done[client_fd]) 
                {
                    if (strstr((char *)buf,"Connection: Upgrade") &&
                        strstr((char *)buf,"Upgrade: websocket"))
                    {
                        char *key_ptr = strstr((char *)buf,"Sec-WebSocket-Key:");
                        if (!key_ptr) continue;

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

                        send(client_fd, resp, strlen(resp), 0);

                        printf("Handshake completed: %d\n\n", client_fd);

                        handshake_done[client_fd] = 1;
                        Player *p = players[client_fd];
                        enqueue_player(p);
                        char msg[] = "{\"type\":\"connected\",\"symbol\":\"Waiting for opponent player ...\"}";
                        send_text_frame(p->player_fd, msg);
                        match_players(); 
                        continue;
                    }
                }
                else 
                {
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
                        continue;
                    }
                    else {
                        mask_offset = 2;
                    }

                    if (opcode == 0x8) {
                        Player *p = players[client_fd];
                        if (p) {
                            if (p->game) {
                                Game *g = p->game;
                                Player *opponent = (g->player1 == p) ? g->player2 : g->player1;
                                if (opponent) {
                                    send_text_frame(opponent->player_fd, "{\"type\":\"opponent_left\"}");
                                    opponent->game = NULL;
                                    opponent->symbol = ' ';
                                    enqueue_player(opponent);
                                }
                                remove_game(g);
                                free(g);
                                match_players();
                            }
                            remove_from_queue(p);
                            players[client_fd] = NULL;
                            free(p);
                        }
                        epoll_ctl(epfd, EPOLL_CTL_DEL, client_fd, NULL);
                        close(client_fd);
                        handshake_done[client_fd] = 0;
                        continue;
                    }

                    if (opcode == 0x1 && mask == 1) {

                        unsigned char masking_key[4];
                        memcpy(masking_key, &buf[mask_offset], 4);

                        unsigned char *payload = &buf[mask_offset + 4];

                        char decoded[BUF_SIZE];
                        for (unsigned long j = 0; j < payload_len; j++) {
                            decoded[j] = payload[j] ^ masking_key[j % 4];
                        }
                        decoded[payload_len] = '\0';

                        Player *p = players[client_fd];
                        char *msg = strndup((char*)decoded, payload_len);
                        if (!msg)
                            break;
                        Game *game = p->game;
                        if (!game) 
                        {
                            free(msg);
                            break;
                        }
                        if (strstr(msg, "\"type\":\"move\"")) 
                        {
                            int pos = -1;
                            sscanf(msg, "{\"type\":\"move\",\"position\":%d}", &pos);
                            if (!game->game_over && pos >= 0 && pos < 9 && game->board[pos] == ' ' && p->symbol == game->current_turn)
                            {
                                game->board[pos] = p->symbol;
                                broadcast_board(game);
                                char winner = check_winner(game);
                                if (winner != ' ')
                                {
                                    game->game_over = 1;
                                    char result_msg[128];
                                    snprintf(result_msg, sizeof(result_msg), "{\"type\":\"result\",\"message\":\"Winner is %c\"}", winner);
                                    send_text_frame(game->player1->player_fd, result_msg);
                                    send_text_frame(game->player2->player_fd, result_msg);
                                    
                                }
                                else if (is_draw(game))
                                {
                                    game->game_over = 1;
                                    char draw_msg[] = "{\"type\":\"result\",\"message\":\"Match Draw\"}";
                                    send_text_frame(game->player1->player_fd, draw_msg);
                                    send_text_frame(game->player2->player_fd, draw_msg);
                                    
                                }
                                game->current_turn = (game->current_turn == 'X') ? 'O' : 'X';
                            }
                        }
                        else if (strstr(msg, "\"type\":\"reset\""))
                        {
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
                    }
                }
            }
        }
    }

    close(sockfd);
    freeaddrinfo(res);
    return 0;
}
