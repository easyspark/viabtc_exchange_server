/*
 * Description: 
 *     History: yang@haipo.me, 2017/04/08, create
 */

# include "me_config.h"
# include "me_message.h"

# include <librdkafka/rdkafka.h>

static rd_kafka_t *rk;

static rd_kafka_topic_t *rkt_deals;
static rd_kafka_topic_t *rkt_orders;
static rd_kafka_topic_t *rkt_balances;

static list_t *list_deals;
static list_t *list_orders;
static list_t *list_balances;

static nw_timer timer;

static void on_delivery(rd_kafka_t *rk, const rd_kafka_message_t *rkmessage, void *opaque)
{
    if (rkmessage->err) {
        log_error("Message delivery failed: %s", rd_kafka_err2str(rkmessage->err));
    } else {
        log_trace("Message delivered (%zd bytes, partition %"PRId32")", rkmessage->len, rkmessage->partition);
    }
}

static void on_logger(const rd_kafka_t *rk, int level, const char *fac, const char *buf)
{
    log_error("RDKAFKA-%i-%s: %s: %s\n", level, fac, rk ? rd_kafka_name(rk) : NULL, buf);
}

static void produce_list(list_t *list, rd_kafka_topic_t *topic)
{
    list_node *node;
    list_iter *iter = list_get_iterator(list, LIST_START_HEAD);
    while ((node = list_next(iter)) != NULL) {
        int ret = rd_kafka_produce(topic, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, node->value, strlen(node->value), NULL, 0, NULL);
        if (ret == -1) {
            log_error("Failed to produce: %s to topic %s: %s\n", (char *)node->value, rd_kafka_topic_name(rkt_deals), rd_kafka_err2str(rd_kafka_last_error()));
            if (rd_kafka_last_error() == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
                break;
            }
        }
        list_del(list, node);
        rd_kafka_poll(rk, 0);
    }
    list_release_iterator(iter);
}

static void on_timer(nw_timer *t, void *privdata)
{
    if (list_deals->len) {
        produce_list(list_deals, rkt_deals);
    }
    if (list_orders->len) {
        produce_list(list_orders, rkt_orders);
    }
    if (list_balances->len) {
        produce_list(list_balances, rkt_balances);
    }

    rd_kafka_poll(rk, 0);
}

static void on_list_free(void *value)
{
    free(value);
}

int init_message(void)
{
    char errstr[1024];
    rd_kafka_conf_t *conf = rd_kafka_conf_new();
    if (rd_kafka_conf_set(conf, "bootstrap.servers", settings.brokers, errstr, sizeof(errstr)) != RD_KAFKA_CONF_OK) {
        log_stderr("Set kafka brokers: %s fail: %s", settings.brokers, errstr);
        return -__LINE__;
    }
    rd_kafka_conf_set_log_cb(conf, on_logger);
    rd_kafka_conf_set_dr_msg_cb(conf, on_delivery);

    rk = rd_kafka_new(RD_KAFKA_PRODUCER, conf, errstr, sizeof(errstr));
    if (rk == NULL) {
        log_stderr("Failed to create new producer: %s", errstr);
        return -__LINE__;
    }

    rkt_deals = rd_kafka_topic_new(rk, "deals", NULL);
    if (rkt_deals == NULL) {
        log_stderr("Failed to create topic object: %s", rd_kafka_err2str(rd_kafka_last_error()));
        return -__LINE__;
    }
    rkt_orders = rd_kafka_topic_new(rk, "orders", NULL);
    if (rkt_orders == NULL) {
        log_stderr("Failed to create topic object: %s", rd_kafka_err2str(rd_kafka_last_error()));
        return -__LINE__;
    }
    rkt_balances = rd_kafka_topic_new(rk, "balances", NULL);
    if (rkt_balances == NULL) {
        log_stderr("Failed to create topic object: %s", rd_kafka_err2str(rd_kafka_last_error()));
        return -__LINE__;
    }

    list_type lt;
    memset(&lt, 0, sizeof(lt));
    lt.free = on_list_free;

    list_deals = list_create(&lt);
    if (list_deals == NULL)
        return -__LINE__;
    list_orders = list_create(&lt);
    if (list_orders == NULL)
        return -__LINE__;
    list_balances = list_create(&lt);
    if (list_balances == NULL)
        return -__LINE__;

    nw_timer_set(&timer, 0.1, true, on_timer, NULL);
    nw_timer_start(&timer);

    return 0;
}

int fini_message(void)
{
    on_timer(NULL, NULL);

    rd_kafka_flush(rk, 1000);
    rd_kafka_topic_destroy(rkt_deals);
    rd_kafka_destroy(rk);

    return 0;
}

static json_t *json_array_append_mpd(json_t *message, mpd_t *val)
{
    char *str = mpd_to_sci(val, 0);
    json_array_append_new(message, json_string(str));
    free(str);
    return message;
}

static int push_message(char *message, rd_kafka_topic_t *topic, list_t *list)
{
    if (list->len) {
        list_add_node_tail(list, message);
        return 0;
    }

    int ret = rd_kafka_produce(rkt_deals, RD_KAFKA_PARTITION_UA, RD_KAFKA_MSG_F_COPY, message, strlen(message), NULL, 0, NULL);
    if (ret == -1) {
        log_error("Failed to produce: %s to topic %s: %s\n", message, rd_kafka_topic_name(rkt_deals), rd_kafka_err2str(rd_kafka_last_error()));
        if (rd_kafka_last_error() == RD_KAFKA_RESP_ERR__QUEUE_FULL) {
            list_add_node_tail(list_deals, message);
            return 0;
        }
        return -__LINE__;
    }
    free(message);
    rd_kafka_poll(rk, 0);

    return 0;
}

int push_deal_message(double t, const char *market, order_t *ask, order_t *bid, mpd_t *price, mpd_t *amount, mpd_t *ask_fee, mpd_t *bid_fee)
{
    json_t *message = json_array();
    json_array_append_new(message, json_real(t));
    json_array_append_new(message, json_string(market));
    json_array_append_new(message, json_integer(ask->id));
    json_array_append_new(message, json_integer(bid->id));
    json_array_append_new(message, json_integer(ask->user_id));
    json_array_append_new(message, json_integer(bid->user_id));
    json_array_append_mpd(message, price);
    json_array_append_mpd(message, amount);
    json_array_append_mpd(message, ask_fee);
    json_array_append_mpd(message, bid_fee);

    push_message(json_dumps(message, 0), rkt_deals, list_deals);
    json_decref(message);

    return 0;
}

bool is_message_block(void)
{
    if (list_deals->len >= MAX_PENDING_MESSAGE)
        return true;
    if (list_orders->len >= MAX_PENDING_MESSAGE)
        return true;
    if (list_balances->len >= MAX_PENDING_MESSAGE)
        return true;

    return false;
}
