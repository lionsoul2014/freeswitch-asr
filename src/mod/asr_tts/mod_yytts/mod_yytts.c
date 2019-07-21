/*
 * tts engine based on yuanyu.ai 
 *
 * chenxin <chenxin619315@gmail.com>
 *
 * mod_yytts.c -- yytts Interface
 *
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <switch.h>
#include <curl/curl.h>


SWITCH_MODULE_LOAD_FUNCTION(mod_yytts_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_yytts_shutdown);
SWITCH_MODULE_DEFINITION(mod_yytts, mod_yytts_load, mod_yytts_shutdown, NULL);

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;

static struct {
    char *api_base;
    char *app_key;
	char *voice;
    char *audio_type;
    int buffer_size;
    int buffer_max_size;
	int auto_reload;
    switch_memory_pool_t *pool;
} globals;

typedef struct yytts {
    char *voice;
	switch_memory_pool_t *pool;
    switch_buffer_t *audio_buffer;
} yytts_t;


static switch_status_t yytts_speech_open(
    switch_speech_handle_t *sh, 
    const char *voice_name, int rate, 
    int channels, switch_speech_flag_t *flags)
{
	yytts_t *yytts = switch_core_alloc(sh->memory_pool, sizeof(yytts_t));

	/// sh->native_rate = 16000;
    if ( voice_name ) {
        yytts->voice = switch_core_strdup(sh->memory_pool, voice_name);
    } else {
        yytts->voice = globals.voice;
    }

    switch_buffer_create_dynamic(&yytts->audio_buffer, 
        globals.buffer_size, globals.buffer_size, globals.buffer_max_size);

    yytts->pool = sh->memory_pool;
	sh->private_info = yytts;


	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t yytts_speech_close(switch_speech_handle_t *sh, switch_speech_flag_t *flags)
{
	yytts_t *yytts = (yytts_t *) sh->private_info;

	if ( yytts->audio_buffer ) {
		switch_buffer_destroy(&yytts->audio_buffer);
	}

	return SWITCH_STATUS_SUCCESS;
}

/**
 * curl write data callback internal function
 *
 * @param   buffer
 * @param   size
 * @param   nmemb
 * @param   ptr
*/
static int wave_recv_callback(void *buffer, size_t size, size_t nmemb, void *ptr)
{
    yytts_t *yytts = (yytts_t *) ptr;
    switch_buffer_write(yytts->audio_buffer, buffer, size * nmemb);
    return size * nmemb;
}

static switch_status_t yytts_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
    char buffer[128] = {0};
    long http_code;
    char *content_type = NULL, *url = NULL;
    size_t url_len = 0;

    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
	yytts_t *yytts = (yytts_t *) sh->private_info;

    // invoke the api to download the text wave data
    curl = curl_easy_init();
    if ( curl == NULL ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "yytts failed to init the curl\n");
        curl_global_cleanup();
        return SWITCH_STATUS_FALSE;
    }

    // make the request url api_base + ?text=${text}&voice=${voice}
    url_len  = strlen(globals.api_base);
    url_len += 37;                          // length for ?app_key=&text=&voice=&audio_type=wav
    url_len += strlen(globals.app_key);     // bytes for app_key
    url_len += strlen(yytts->voice);
    url = (char *) switch_core_alloc(yytts->pool, url_len + 1);
    strcat(url, globals.api_base);
    strcat(url, "?app_key=");
    strcat(url, globals.app_key);
    strcat(url, "&text=");
    strcat(url, text);
    strcat(url, "&voice=");
    strcat(url, yytts->voice);
    strcat(url, "&audio_type=");
    strcat(url, globals.audio_type);
	/// switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "yytts.feed_tts url=%s\n", url);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "GET");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000);          // 15 seconds
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 36000);   // 10 hours for DNS cache


    // initialized the audio_buffer and set the recv callback
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, wave_recv_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, yytts);

    // make the request header
    snprintf(buffer, sizeof(buffer), "Content-Type: application/json");
    headers = curl_slist_append(headers, buffer);
    snprintf(buffer, sizeof(buffer), "User-Agent: mod_yytts/freeswitch/0.1 yuanyu.ai");
    headers = curl_slist_append(headers, buffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // perform the request
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    curl_slist_free_all(headers);
    if ( http_code != 200 ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "yytts feed failed to perform the request\n");
        curl_easy_cleanup(curl);
        return SWITCH_STATUS_FALSE;
    }

    if ( content_type == NULL || strstr(content_type, "audio/") == NULL ) {
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return SWITCH_STATUS_FALSE;
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
	return SWITCH_STATUS_SUCCESS;
}

static void yytts_speech_flush_tts(switch_speech_handle_t *sh)
{
	yytts_t *yytts = (yytts_t *) sh->private_info;

	if ( yytts->audio_buffer ) {
	    switch_buffer_zero(yytts->audio_buffer);
	}
}

static switch_status_t yytts_speech_read_tts(switch_speech_handle_t *sh, void *data, size_t *datalen, switch_speech_flag_t *flags)
{
	size_t bytes_read;
	yytts_t *yytts = (yytts_t *) sh->private_info;

	///  if ( ! yytts->audio_buffer 
    ///      || switch_buffer_inuse(yytts->audio_buffer) == 0 ) {
    ///      return SWITCH_STATUS_FALSE;
	///  }

	if ( (bytes_read = switch_buffer_read(yytts->audio_buffer, data, *datalen)) ) {
		*datalen = bytes_read;
		return SWITCH_STATUS_SUCCESS;
	}

	return SWITCH_STATUS_FALSE;
}

static void yytts_text_param_tts(switch_speech_handle_t *sh, char *param, const char *val)
{

}

static void yytts_numeric_param_tts(switch_speech_handle_t *sh, char *param, int val)
{

}

static void yytts_float_param_tts(switch_speech_handle_t *sh, char *param, double val)
{

}

/* internal function to load the yytts configuration*/
static switch_status_t yytts_load_config(void)
{
    char *cf = "yytts.conf";
    switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

    // global default settings
    globals.api_base = "http://sebiengine.ai/api/acoustics/tts/";
    globals.app_key = NULL;
    globals.voice = "zhilingf";
    globals.audio_type = "wav";
    globals.buffer_size = 49152;
    globals.buffer_max_size = 4194304;
    globals.auto_reload = 1;

    if ( ! (xml = switch_xml_open_cfg(cf, &cfg, NULL)) ) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Open of %s failed\n", cf);
		status = SWITCH_STATUS_FALSE;
		goto done;
    }

    if ( (settings = switch_xml_child(cfg, "settings")) ) {
		for ( param = switch_xml_child(settings, "param"); param; param = param->next ) {
			char *var = (char *) switch_xml_attr_soft(param, "name");
			char *val = (char *) switch_xml_attr_soft(param, "value");
            if ( strcasecmp(var, "app-key") == 0 ) {
				globals.app_key = switch_core_strdup(globals.pool, val);
			} else if ( strcasecmp(var, "voice") == 0 ) {
				globals.voice = switch_core_strdup(globals.pool, val);
			} else if ( strcasecmp(var, "audio-type") == 0 ) {
				globals.audio_type = switch_core_strdup(globals.pool, val);
			} else if ( strcasecmp(var, "buffer-size") == 0 ) {
				globals.buffer_size = atoi(val);
			} else if ( strcasecmp(var, "buffer-max-size") == 0 ) {
				globals.buffer_max_size = atoi(val);
			} else if ( strcasecmp(var, "audo-reload") == 0 ) {
				globals.auto_reload = switch_true(val);
			}
		}
	}

    done:
        if (xml) {
		    switch_xml_free(xml);
	    }

    return status;
}


static void do_config_load(void)
{
    switch_mutex_lock(MUTEX);
	yytts_load_config();
	switch_mutex_unlock(MUTEX);
}

static void event_handler(switch_event_t *event)
{
    if ( globals.auto_reload ) {
        do_config_load();
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "yytts reloaded\n");
    }
}

SWITCH_MODULE_LOAD_FUNCTION(mod_yytts_load)
{
	switch_speech_interface_t *tts_interface;

    switch_mutex_init(&MUTEX, SWITCH_MUTEX_NESTED, pool);
    globals.pool = pool;

    if ( (switch_event_bind_removable(modname, SWITCH_EVENT_RELOADXML, 
        NULL, event_handler, NULL, &NODE) != SWITCH_STATUS_SUCCESS) ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind reloadxml event!\n");
	}

    do_config_load();

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);
	tts_interface  = switch_loadable_module_create_interface(*module_interface, SWITCH_SPEECH_INTERFACE);
	tts_interface->interface_name = "yytts";
	tts_interface->speech_open = yytts_speech_open;
	tts_interface->speech_close = yytts_speech_close;
	tts_interface->speech_feed_tts = yytts_speech_feed_tts;
	tts_interface->speech_read_tts = yytts_speech_read_tts;
	tts_interface->speech_flush_tts = yytts_speech_flush_tts;
	tts_interface->speech_text_param_tts = yytts_text_param_tts;
	tts_interface->speech_numeric_param_tts = yytts_numeric_param_tts;
	tts_interface->speech_float_param_tts = yytts_float_param_tts;

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_yytts_shutdown)
{
	switch_event_unbind(&NODE);
	return SWITCH_STATUS_UNLOAD;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4 noet:
 */
