#include <libwebsockets.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>

#define MAX_QUEUE 10

typedef struct game_id game_id_t;
typedef struct client client_t;

typedef struct game_id {
    client_t *player1;
    client_t *player2;
    char board[9];
    char current_turn;
    int game_over;
    struct game_id *next;   
} game_id_t;

typedef struct client {
    struct lws *wsi; 
    char symbol;
    game_id_t *id;
    char messages[MAX_QUEUE][512];
    int msg_len[MAX_QUEUE];
    int head;
    int tail;
    int enqueued; 
    int connected;
} client_t;

typedef struct waiting_node {
    client_t *client;
    struct waiting_node *next;
} waiting_node_t;

void enqueue_player(client_t *c);
static waiting_node_t *queue_head = NULL;
static waiting_node_t *queue_tail = NULL;
static game_id_t *ids = NULL;
static int interrupted = 0;

void sigint_handler(int sig) 
{
    interrupted = 1;
}
void send_text(struct lws *wsi, const char *msg)
{
    if (!wsi) 
        return;
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

void broadcast_board(game_id_t *id)
{
    char msg[512];
    snprintf(msg, sizeof(msg), "{\"type\":\"update\",\"board\":[\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\",\"%c\"]}", id->board[0], id->board[1], id->board[2], id->board[3], id->board[4], id->board[5], id->board[6], id->board[7], id->board[8]);
    if (id->player1 && id->player1->wsi)
        send_text(id->player1->wsi, msg);
    if (id->player2 && id->player2->wsi)
        send_text(id->player2->wsi, msg);
}

char check_winner(game_id_t *id)
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
        if (id->board[a] != ' ' && id->board[a] == id->board[b] && id->board[a] == id->board[c])
        {
            return id->board[a];
        }
    }
    return ' ';
}

int is_draw(game_id_t *id)
{
    for (int i = 0; i < 9; i++)
    {
        if (id->board[i] == ' ')
            return 0;
    }
    return 1;
}

void remove_game(game_id_t *id)
{
    game_id_t **curr = &ids;
    while (*curr)
    {
        if (*curr == id)
        {
            *curr = id->next;
            return;
        }
        curr = &((*curr)->next);
    }
}

client_t *dequeue_player()
{
    if (!queue_head)
        return NULL;
    waiting_node_t *node = queue_head;
    client_t *c = node->client;
    queue_head = node->next;
    if (!queue_head)
        queue_tail = NULL;
    free(node);
    c->enqueued = 0;
    return c;
}

void match_players()
{
    while (queue_head && queue_head->next)
    {
        client_t *p1 = dequeue_player();
        client_t *p2 = dequeue_player();
        if (!p1->connected)
        {
            enqueue_player(p2);
            continue;
        }
        if (!p2->connected)
        {
            enqueue_player(p1);
            continue;
        }
        game_id_t *id = malloc(sizeof(game_id_t));
        memset(id, 0, sizeof(game_id_t));
        for (int i = 0; i < 9; i++)
            id->board[i] = ' ';
        id->player1 = p1;
        id->player2 = p2;
        id->current_turn = 'X';
        p1->symbol = 'X';
        p2->symbol = 'O';
        p1->id = id;
        p2->id = id;
        id->next = ids;
        ids = id;
        send_text(p1->wsi, "{\"type\":\"assign\",\"symbol\":\"You are player X !\"}");
        send_text(p2->wsi, "{\"type\":\"assign\",\"symbol\":\"You are player O !\"}");
        broadcast_board(id);
    }
}

void enqueue_player(client_t *c)
{
    if (c->enqueued) 
        return; 
    waiting_node_t *node = malloc(sizeof(waiting_node_t));
    node->client = c;
    node->next = NULL;
    if (!queue_tail)
        queue_head = queue_tail = node;
    else
    {
        queue_tail->next = node;
        queue_tail = node;
    }
    c->enqueued = 1;
}

void remove_from_queue(client_t *c)
{
    waiting_node_t **curr = &queue_head;
    waiting_node_t *prev = NULL;
    while (*curr)
    {
        waiting_node_t *node = *curr;
        if (node->client == c)
        {
            *curr = node->next;
            if (node == queue_tail)
                queue_tail = prev;
            c->enqueued = 0;
            free(node);
            return;
        }
        prev = node;
        curr = &node->next;
    }
}
static int callback_json(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) 
{
    client_t *c = (client_t *)user;
    switch (reason) 
    {
       case LWS_CALLBACK_ESTABLISHED:
       {
          c->wsi = wsi;
          c->head = c->tail = 0;
          c->id = NULL;
          c->symbol = ' ';
          c->enqueued = 0;
          c->connected = 1;
          enqueue_player(c);
          char msg[] = "{\"type\":\"assign\",\"symbol\":\"Waiting for opponent player ...\"}";
          send_text(c->wsi, msg);
          match_players(); 
             
        }
        break; 
        
        case LWS_CALLBACK_RECEIVE:
        {
            char *msg = strndup((char*)in, len);
            if (!msg)
                break;
            game_id_t *id = c->id;
            if (!id) 
            {
                free(msg);
                break;
            }
            if (strstr(msg, "\"type\":\"move\"")) 
            {
                int pos = -1;
                sscanf(msg, "{\"type\":\"move\",\"position\":%d}", &pos);
                if (!id->game_over && pos >= 0 && pos < 9 && id->board[pos] == ' ' && c->symbol == id->current_turn)
                {
                    id->board[pos] = c->symbol;
                    broadcast_board(id);
                    char winner = check_winner(id);
                    if (winner != ' ')
                    {
                        id->game_over = 1;
                        char result_msg[128];
                        snprintf(result_msg, sizeof(result_msg), "{\"type\":\"result\",\"message\":\"Winner is %c\"}", winner);
                        send_text(id->player1->wsi, result_msg);
                        send_text(id->player2->wsi, result_msg);
                        free(msg);
                        break;
                    }
                    if (is_draw(id))
                    {
                        id->game_over = 1;
                        char draw_msg[] = "{\"type\":\"result\",\"message\":\"Match Draw\"}";
                        send_text(id->player1->wsi, draw_msg);
                        send_text(id->player2->wsi, draw_msg);
                        free(msg);
                        break;
                    }
                    id->current_turn = (id->current_turn == 'X') ? 'O' : 'X';
                }
            }
            else if (strstr(msg, "\"type\":\"reset\""))
            {
                for (int i = 0; i < 9; i++)
                id->board[i] = ' ';
                id->current_turn = 'X';
                id->game_over = 0;
                broadcast_board(id);
                char reset_msg[] = "{\"type\":\"reset\"}";
                send_text(id->player1->wsi, reset_msg);
                send_text(id->player2->wsi, reset_msg);
            }
            free(msg);
            break;
        }
   
        case LWS_CALLBACK_CLOSED:
        {
            c->wsi = NULL;
            remove_from_queue(c);
            c->enqueued = 0;
            c->connected = 0;
            game_id_t *id = c->id;
            if (id)
            {
                client_t *remaining = (id->player1 == c) ? id->player2 : id->player1;
                if (remaining && remaining->wsi && remaining->connected)
                {
                    char msg[] = "{\"type\":\"opponent_left\"}";
                    send_text(remaining->wsi, msg);
                    remaining->id = NULL;
                    remaining->symbol = ' ';
                    if (!remaining->enqueued)
                    {
                        enqueue_player(remaining);
                        match_players(); 
                    }
                }
                remove_game(id);
                id->next = NULL;
                free(id);
            }
            c->id = NULL;
            c->symbol = ' ';
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

