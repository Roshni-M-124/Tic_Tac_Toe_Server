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
    lws_callback_on_writable(wsi);
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

static int callback_json(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) 
{
    client_t *c = (client_t *)user;
    switch (reason) 
    {
        case LWS_CALLBACK_ESTABLISHED:
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
            
        case LWS_CALLBACK_RECEIVE:
            char *msg = strndup((char*)in, len);
            printf("Received message: %s\n", msg);
            if (strstr(msg, "\"type\":\"move\"")) 
            {
                int pos = -1;
                sscanf(msg, "{\"type\":\"move\",\"position\":%d}", &pos);
                printf("pos=%d board[pos]=%c symbol=%c current_turn=%c\n", pos, (pos >=0 && pos <9) ?board[pos]:'?', c->symbol,
       current_turn);
                if (pos >= 0 && pos < 9 && board[pos] == ' ' && c->symbol != ' ' && c->symbol == current_turn) 
                {
                    board[pos] = c->symbol; 
                    current_turn = (current_turn == 'X') ? 'O' : 'X';
                    broadcast_board();        
                }
            }
            else if (strstr(msg, "\"type\":\"reset\"")) 
            {
                for (int i = 0; i < 9; i++) 
                    board[i] = ' ';
                current_turn = 'X';
                broadcast_board();            
            }
            free(msg);
            break;
            
        case LWS_CALLBACK_CLOSED:
            printf("Client disconnected!\n");
            if (player1 == c) player1 = NULL;
            if (player2 == c) player2 = NULL;
            c->symbol = ' ';
            break;
            
        case LWS_CALLBACK_SERVER_WRITEABLE:
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

