#include "prpltwtr_endpoint_search.h"

static gpointer twitter_search_timeout_context_new(GHashTable * components)
{
    TwitterSearchTimeoutContext *ctx = g_slice_new0(TwitterSearchTimeoutContext);
    ctx->search_name = g_strdup(g_hash_table_lookup(components, "search"));
    return ctx;
}

static void twitter_search_timeout_context_free(gpointer _ctx)
{
    TwitterSearchTimeoutContext *ctx;
    purple_debug_info(GENERIC_PROTOCOL_ID, "%s\n", G_STRFUNC);
    g_return_if_fail(_ctx != NULL);
    ctx = _ctx;

    purple_debug_info(GENERIC_PROTOCOL_ID, "%s %s\n", G_STRFUNC, ctx->search_name);
    g_free(ctx->search_name);
    ctx->search_name = NULL;

    g_free(ctx->last_tweet_id);
    ctx->last_tweet_id = NULL;

    g_slice_free(TwitterSearchTimeoutContext, ctx);
}

static char    *twitter_chat_name_from_search(const char *search)
{
#ifdef _HAZE_
    return g_strdup_printf("#%s", search);
#else
    char           *search_lower = g_utf8_strdown(search, -1);
    char           *chat_name = g_strdup_printf("Search: %s", search_lower);
    g_free(search_lower);
    return chat_name;
#endif
}

static char    *twitter_search_chat_name_from_components(GHashTable * components)
{
    const char     *search = g_hash_table_lookup(components, "search");
    return twitter_chat_name_from_search(search);
}

static void twitter_get_search_parse_statuses(TwitterEndpointChat * endpoint_chat, GList * statuses)
{
    GList          *l;
    TwitterUserTweet *user_tweet;

    purple_debug_info(purple_account_get_protocol_id(endpoint_chat->account), "%s\n", G_STRFUNC);

    g_return_if_fail(endpoint_chat != NULL);
    purple_account_get_connection(endpoint_chat->account);

    if (!statuses) {
        /* At least update the topic with the new rate limit info */
        twitter_chat_update_rate_limit(endpoint_chat);
        return;
    }

    l = g_list_last(statuses);
    user_tweet = l->data;
    if (user_tweet && user_tweet->status) {
        TwitterSearchTimeoutContext *ctx = endpoint_chat->endpoint_data;
        gchar          *key = g_strdup_printf("search_%s", ctx->search_name);
        ctx->last_tweet_id = g_strdup(user_tweet->status->id);
        purple_account_set_string(endpoint_chat->account, key, ctx->last_tweet_id);
        g_free(key);
    }
    twitter_chat_got_user_tweets(endpoint_chat, statuses);
}

static gchar   *twitter_search_verify_components(GHashTable * components)
{
    const char     *search = g_hash_table_lookup(components, "search");
    if (search == NULL || search[0] == '\0') {
        return g_strdup(_("Search must be filled in when joining a search chat"));
    }
    return NULL;
}

static gboolean twitter_get_search_all_error_cb(TwitterRequestor * r, const TwitterRequestErrorData * error_data, gpointer user_data)
{
    TwitterEndpointChatId *chat_id = (TwitterEndpointChatId *) user_data;
    TwitterEndpointChat *endpoint_chat;

    purple_debug_warning(purple_account_get_protocol_id(r->account), "%s(%p): %s\n", G_STRFUNC, user_data, error_data->message);

    g_return_val_if_fail(chat_id != NULL, TRUE);
    endpoint_chat = twitter_endpoint_chat_find_by_id(chat_id);
    twitter_endpoint_chat_id_free(chat_id);

    if (endpoint_chat) {
        endpoint_chat->retrieval_in_progress = FALSE;
        endpoint_chat->retrieval_in_progress_timeout = 0;
    }

    return FALSE;                                /* Do not retry. Too many edge cases */
}

static void twitter_get_search_all_cb(TwitterRequestor * r, GList * nodes, gpointer user_data)
{
    TwitterEndpointChatId *chat_id = (TwitterEndpointChatId *) user_data;
    TwitterEndpointChat *endpoint_chat;
    GList          *statuses;

    purple_debug_info(purple_account_get_protocol_id(r->account), "%s\n", G_STRFUNC);

    g_return_if_fail(chat_id != NULL);
    endpoint_chat = twitter_endpoint_chat_find_by_id(chat_id);
    twitter_endpoint_chat_id_free(chat_id);

    if (endpoint_chat == NULL)
        return;

    endpoint_chat->rate_limit_remaining = r->rate_limit_remaining;
    endpoint_chat->rate_limit_total = r->rate_limit_total;

    endpoint_chat->retrieval_in_progress = FALSE;
    endpoint_chat->retrieval_in_progress_timeout = 0;

    statuses = twitter_statuses_nodes_parse(r, nodes);
    twitter_get_search_parse_statuses(endpoint_chat, statuses);
}

static gboolean twitter_search_timeout(TwitterEndpointChat * endpoint_chat)
{
    PurpleAccount  *account = endpoint_chat->account;
    TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *) endpoint_chat->endpoint_data;
    TwitterEndpointChatId *chat_id = twitter_endpoint_chat_id_new(endpoint_chat);
    gchar          *key = g_strdup_printf("search_%s", ctx->search_name);

    ctx->last_tweet_id = g_strdup(purple_account_get_string(endpoint_chat->account, key, NULL));
    g_free(key);

    purple_debug_info(purple_account_get_protocol_id(account), "Resuming search for %s from %s\n", ctx->search_name, ctx->last_tweet_id);

    if (endpoint_chat->retrieval_in_progress && endpoint_chat->retrieval_in_progress_timeout <= 0) {
        purple_debug_warning(purple_account_get_protocol_id(account), "There was a retreival in progress, but it appears dead. Ignoring it\n");
        endpoint_chat->retrieval_in_progress = FALSE;
    }

    if (endpoint_chat->retrieval_in_progress) {
        purple_debug_warning(purple_account_get_protocol_id(account), "Skipping retreival for %s because one is already in progress!\n", account->username);
        endpoint_chat->retrieval_in_progress_timeout--;
        return TRUE;
    }

    endpoint_chat->retrieval_in_progress = TRUE;
    endpoint_chat->retrieval_in_progress_timeout = 2;

    if (!ctx->last_tweet_id || !strcmp(ctx->last_tweet_id, "0")) {
        purple_debug_info(purple_account_get_protocol_id(account), "Retrieving %s statuses for first time\n", ctx->search_name);
    } else {
        purple_debug_info(purple_account_get_protocol_id(account), "Retrieving %s statuses since %s\n", ctx->search_name, ctx->last_tweet_id);
    }
    twitter_api_get_search_all(purple_account_get_requestor(account), ctx->search_name, ctx->last_tweet_id, twitter_get_search_all_cb, twitter_get_search_all_error_cb, TWITTER_SEARCH_COUNT_DEFAULT, chat_id);

    return TRUE;
}

static gboolean twitter_endpoint_search_interval_start(TwitterEndpointChat * endpoint_chat)
{
    return twitter_search_timeout(endpoint_chat);
}

static gchar   *twitter_endpoint_search_get_status_added_text(TwitterEndpointChat * endpoint_chat)
{
    TwitterSearchTimeoutContext *ctx = (TwitterSearchTimeoutContext *) endpoint_chat->endpoint_data;
    return g_strdup(ctx->search_name);
}

static TwitterEndpointChatSettings TwitterEndpointSearchSettings = {
    TWITTER_CHAT_SEARCH,
#ifdef _HAZE_
    '#',
#endif
    twitter_endpoint_search_get_status_added_text,
    twitter_search_timeout_context_free,         //endpoint_data_free
    twitter_option_search_timeout,               //get_default_interval
    twitter_search_chat_name_from_components,    //get_name
    twitter_search_verify_components,            //verify_components
    twitter_search_timeout,                      //interval_timeout
    twitter_endpoint_search_interval_start,
    twitter_search_timeout_context_new,
};

TwitterEndpointChatSettings *twitter_endpoint_search_get_settings(void)
{
    return &TwitterEndpointSearchSettings;
}
