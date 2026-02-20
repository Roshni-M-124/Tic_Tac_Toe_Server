#include <libwebsockets.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>

#define MAX_QUEUE 10

typedef struct {
    struct lws *wsi; 
    char symbol;
    char messages[MAX_QUEUE][512];
    int msg_len[MAX_QUEUE];
    int head;
    int tail;
} client_t;

static int game_over = 0;
static int interrupted = 0;
static char current_turn = 'X';
static char board[9] = {' ',' ',' ',' ',' ',' ',' ',' ',' '};
static client_t *player1 = NULL;
static client_t *player2 = NULL;

void sigint_handler(int sig) 
{
    interrupted = 1;
}
void send_text(struct lws *wsi, const char *msg)
{
    client_t *c = (client_t *)lws_wsi_user(wsi);
    int next = (c->tail + 1) % MAX_QUEUE;
    if (next == c->head)
    {
        lwsl_err("Message queue full\n");
        return;
    }
    size_t len = strlen(msg);
    if (len > 511) len = 511;
    memcpy(c->messages[c->tail], msg, len);
    c->messages[c->tail][len] = '\0';
    c->msg_len[c->tail] = len;
    c->tail = next;
    lws_callback_on_writable_all_protocol(
        lws_get_context(wsi),
        lws_get_protocol(wsi)
    );
}

void broadcast_board()
{
    char msg[512];
    snprintf(msg, sizeof(msg), "{\"type\":\"update\",\"board\":[\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\"]}", board[0], board[1], board[2], board[3], board[4], board[5], board[6], board[7], board[8]);
    if (player1 && player1->wsi)
        send_text(player1->wsi, msg);
    if (player2 && player2->wsi)
        send_text(player2->wsi, msg);
}

char check_winner()
{
    int wins[8][3] = { {0,1,2}, {3,4,5}, {6,7,8}, {0,3,6}, {1,4,7}, {2,5,8}, {0,4,8}, {2,4,6}};
    for (int i = 0; i < 8; i++)
    {
        int a = wins[i][0];
        int b = wins[i][1];
        int c = wins[i][2];
        if (board[a] != ' ' && board[a] == board[b] && board[a] == board[c])
        {
            return board[a];   
        }
    }
    return ' '; 
}

int is_draw()
{
    for (int i = 0; i < 9; i++)
    {
        if (board[i] == ' ')
            return 0;  
    }
    return 1;  
}

static int callback_json(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) 
{
    client_t *c = (client_t *)user;
    switch (reason) 
    {
        case LWS_CALLBACK_ESTABLISHED:
        {
            printf("Client connected!\n");
            c->wsi = wsi; 
            c->head = 0;
            c->tail = 0;
            if (!player1)
            {
                player1 = c;
                c->symbol = 'X';
                char msg[128];
                snprintf(msg, sizeof(msg), "{\"type\":\"assign\",\"symbol\":\"%c\"}", c->symbol);
                send_text(wsi, msg);
            } 
            else if (!player2) 
            {
                player2 = c;
                c->symbol = 'O';
                char msg[128];
                snprintf(msg, sizeof(msg), "{\"type\":\"assign\",\"symbol\":\"%c\"}", c->symbol);
                send_text(wsi, msg);
            } 
            else 
            {
                c->symbol = ' '; 
            }
            if (player1 && player2) 
            {
                broadcast_board();
            }
            break;
        }
            
        case LWS_CALLBACK_RECEIVE:
        {
            char *msg = strndup((char*)in, len);
            printf("Received message: %s\n", msg);
            if (strstr(msg, "\"type\":\"move\"")) 
            {
                int pos = -1;
                sscanf(msg, "{\"type\":\"move\",\"position\":%d}", &pos);
                printf("pos=%d board[pos]=%c symbol=%c current_turn=%c\n", pos, (pos >=0 && pos <9) ?board[pos]:'?', c->symbol,
       current_turn);
                if (!game_over && pos >= 0 && pos < 9 && board[pos] == ' ' && c->symbol != ' ' && c->symbol == current_turn) 
                {
                    board[pos] = c->symbol;
                    broadcast_board();   
                    char winner = check_winner();
                    if (winner != ' ')
                    {
                        char result_msg[128];
                        game_over = 1;
                        snprintf(result_msg, sizeof(result_msg), "{\"type\":\"result\",\"message\":\"Winner is %c\"}", winner);
                        if (player1 && player1->wsi)
                            send_text(player1->wsi, result_msg);
                        if (player2 && player2->wsi)
                            send_text(player2->wsi, result_msg);
                        free(msg); 
                        break; 
                    }
                    if (is_draw())
                    {
                        char draw_msg[] = "{\"type\":\"result\",\"message\":\"Match Draw\"}";
                        if (player1 && player1->wsi)
                            send_text(player1->wsi, draw_msg);
                        if (player2 && player2->wsi)
                            send_text(player2->wsi, draw_msg);
                        game_over = 1;
                        free(msg); 
                        break;
                    }
                    current_turn = (current_turn == 'X') ? 'O' : 'X';  
                }
            }
            else if (strstr(msg, "\"type\":\"reset\"")) 
            {
                for (int i = 0; i < 9; i++) 
                    board[i] = ' ';
                current_turn = 'X';
                game_over = 0;
                broadcast_board(); 
                char reset_msg[] = "{\"type\":\"reset\"}";
                if (player1 && player1->wsi)
                    send_text(player1->wsi, reset_msg);
                if (player2 && player2->wsi)
                    send_text(player2->wsi, reset_msg);
            }
            free(msg);
            break;
        }
            
        case LWS_CALLBACK_CLOSED:
        {
            printf("Client disconnected!\n");
            client_t *remaining = NULL;
            if (player1 == c) 
            {
                player1 = NULL;
                remaining = player2;
            } 
            else if (player2 == c) 
            {
                player2 = NULL;
                remaining = player1;
            }
            c->symbol = ' ';
            for (int i = 0; i < 9; i++)
                board[i] = ' ';
            current_turn = 'X';
            game_over = 0;
            if (remaining && remaining->wsi)
            {
                char msg[] = "{\"type\":\"opponent_left\"}";
                send_text(remaining->wsi, msg);
            }
            break;
        }
            
        case LWS_CALLBACK_SERVER_WRITEABLE:
        {
            if (c->head != c->tail)
            {
                unsigned char buf[LWS_PRE + 512];
                int len = c->msg_len[c->head];
                memcpy(buf + LWS_PRE, c->messages[c->head], len);
                int n = lws_write(wsi, buf + LWS_PRE, len, LWS_WRITE_TEXT);
                if (n < 0) 
                {
                    lwsl_err("lws_write failed\n");
                    return -1;
                }
                c->head = (c->head + 1) % MAX_QUEUE;
                if (c->head != c->tail)
                lws_callback_on_writable(wsi);
            }
            break;
        }
            
        default:
            break;
    }
    return 0;
}

static const struct lws_protocols protocols[] = {
    {"", callback_json, sizeof(client_t), 1024},
    {NULL, NULL, 0, 0} 
};

int main()
{
    struct lws_context_creation_info info;
    struct lws_context *context;
    memset(&info, 0, sizeof(info));
    signal(SIGINT, sigint_handler);
    info.port = 8080;           
    info.protocols = protocols;  
    info.options |= LWS_SERVER_OPTION_IPV6;
    info.gid = -1;               
    info.uid = -1;               
    context = lws_create_context(&info);
    if (!context) 
    {
        fprintf(stderr, "Failed to create context\n");
        return 1;
    }
    printf("WebSocket server running on port 8080...\n");
    while (!interrupted)
    {
        lws_service(context, 1000);
    }
    lws_context_destroy(context);
    printf("Server stopped.\n");
    return 0;
}

