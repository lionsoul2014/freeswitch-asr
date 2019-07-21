/*
 * yyasr asr&nlu module based on yuanyu.ai
 *
 * chenxin <chenxin619315@gmail.com>
 *
 * mod_yyasr.c - yyasr Interface
 *
*/

#include <string.h>
#include <switch.h>
#include <switch_cJSON.h>
#include <curl/curl.h>
#include <time.h>

#define WAVE_FILE_LENGTH 126
// 126 - 16(/time.pcm)
#define WAVE_DIR_MAX_LENGTH 110

SWITCH_MODULE_LOAD_FUNCTION(mod_yyasr_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_yyasr_shutdown);
SWITCH_MODULE_DEFINITION(mod_yyasr, mod_yyasr_load, mod_yyasr_shutdown, NULL);

static switch_mutex_t *MUTEX = NULL;
static switch_event_node_t *NODE = NULL;

static struct {
    char *api_url;
    char *app_key;
    char *engine;
    char *model;
    switch_bool_t vad;
    switch_bool_t nlu;

    /* for vad */
    int silence_avg_threshold;
    int silence_max_threshold;
    int feed_min_avg_energy;
    int feed_min_max_energy;
    int no_input_hangover;
    int silence_hangover;
    int min_listen_hits;

    /* for audio and text buffer*/
    int audio_buffer_size;
    int audio_buffer_max_size;
    int text_buffer_size;
    int text_buffer_max_size;
    int nlu_buffer_size;
    int nlu_buffer_max_size;

    /* for debug */
    char *wav_file_dir;
	int auto_reload;
    switch_memory_pool_t *pool;
} globals;


typedef enum {
	YY_FLAG_READY = (1 << 0),
	YY_FLAG_INPUT_TIMERS = (1 << 1),
	YY_FLAG_NOINPUT_TIMEOUT = (1 << 2),
	YY_FLAG_SPEECH_TIMEOUT = (1 << 3),
	YY_FLAG_RECOGNITION = (1 << 4),
	YY_FLAG_HAS_TEXT = (1 << 5),
} yyasr_flag_t;

typedef struct yyasr {
    uint32_t flags;
    switch_mutex_t *flag_mutex;
    switch_memory_pool_t *pool;

    switch_buffer_t *text_buffer;
    switch_buffer_t *audio_buffer;
    size_t audio_size;
    int listen_hits;
    int hangover_hits;
} yyasr_t;


/**
 * get the switch buffer ptr which is safe for std string operation
*/
static char *get_switch_buffer_ptr(switch_buffer_t *buffer)
{
    char *ptr = switch_buffer_get_head_pointer(buffer);
    ptr[switch_buffer_inuse(buffer)] = '\0';
    return ptr;
}

/**
 * rest ASR text recv callbcak function
 *
 * @param   buffer
 * @param   size
 * @param   nmemb
 * @param   ptr
*/
static int rest_recv_callback(void *buffer, size_t size, size_t nmemb, void *ptr)
{
    yyasr_t *yyasr = (yyasr_t *) ptr;
    switch_buffer_write(yyasr->text_buffer, buffer, size * nmemb);
    return size * nmemb;
}

/**
 * invoke the rest api to convert the wav to text
 *
 * @param   yyasr
 * @return  bool
*/
static switch_bool_t recognition_by_rest_api(yyasr_t *yyasr)
{
    char buffer[128] = {0};
    long http_code;
    char *content_type = NULL;

    CURL *curl = NULL;
    struct curl_slist *headers = NULL;
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;

    /* for JSON result parse*/
    cJSON *json_result = NULL;
    cJSON *cursor = NULL;


    // invoke the api to download the text wave data
    curl = curl_easy_init();
    if ( curl == NULL ) {
	    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "yyasr failed to init the curl\n");
        curl_global_cleanup();
        return SWITCH_FALSE;
    }


    curl_easy_setopt(curl, CURLOPT_URL, globals.api_url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000);          // 15 seconds
    curl_easy_setopt(curl, CURLOPT_DNS_CACHE_TIMEOUT, 36000);   // 10 hours for DNS cache

    // try to update the buffer wav data
    curl_formadd(
        &formpost, &lastptr,
        CURLFORM_COPYNAME, "file",
        CURLFORM_BUFFER, "fs-record.wav",
        CURLFORM_BUFFERPTR, get_switch_buffer_ptr(yyasr->audio_buffer),
        CURLFORM_BUFFERLENGTH, switch_buffer_inuse(yyasr->audio_buffer),
        CURLFORM_CONTENTTYPE, "audio/wav",
        CURLFORM_END
    );
    curl_easy_setopt(curl, CURLOPT_HTTPPOST, formpost);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rest_recv_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, yyasr);

    // make the request header
    snprintf(buffer, sizeof(buffer), "Content-Type: multipart/form-data");
    headers = curl_slist_append(headers, buffer);
    snprintf(buffer, sizeof(buffer), "User-Agent: mod_yyasr/freeswitch/0.1 yuanyu.ai");
    headers = curl_slist_append(headers, buffer);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    // perform the request
    curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_getinfo(curl, CURLINFO_CONTENT_TYPE, &content_type);
    curl_formfree(formpost);
    curl_slist_free_all(headers);
    if ( http_code != 200 ) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, 
            "yyasr feed failed to perform the request\n");
        curl_easy_cleanup(curl);
        return SWITCH_FALSE;
    }

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
    //     "yyasr.api_result=%s\n", get_switch_buffer_ptr(yyasr->text_buffer));
    /* Parse the json result and get the asr result */
    json_result = cJSON_Parse(get_switch_buffer_ptr(yyasr->text_buffer));
    if ( json_result == NULL ) {
        return SWITCH_FALSE;
    }
    
    switch_buffer_zero(yyasr->text_buffer);
    cursor = cJSON_GetObjectItem(json_result, "errno");
    if ( cursor != NULL && cursor->valueint == 0 ) {
        cursor = cJSON_GetObjectItem(
            cJSON_GetObjectItem(json_result, "data"), "text");
        // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
        //     "yyasr.asr_text=%s\n", cursor->valuestring);
        switch_buffer_write(yyasr->text_buffer, 
            cursor->valuestring, strlen(cursor->valuestring));
    }

    cJSON_Delete(json_result);
    curl_easy_cleanup(curl);
    curl_global_cleanup();
    return SWITCH_TRUE;
}

/**
 * internal function to recv the audio input and do the stop detect checking
 *
 * @param   ah
 * @param   data
 * @param   samples
*/
static switch_bool_t stop_audio_detect(
    switch_asr_handle_t *ah, int16_t *data, unsigned int samples)
{
    int16_t one_sample;
    register int16_t abs_sample;
    double energy = 0;
    uint32_t c = 0, avg_energy, max_energy, resamples;
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;


    // simple sample energy threshold for VAD
    abs_sample = abs(data[0]);
    energy = abs_sample;
    max_energy = abs_sample;
    for ( c = 1; c < samples; c++ ) {
        abs_sample = abs(data[c]);
        energy += abs_sample;
        if ( abs_sample > max_energy ) {
            max_energy = abs_sample;
        }
    }

    avg_energy = (uint32_t) (energy / samples);
    if ( avg_energy > globals.silence_avg_threshold 
        || max_energy > globals.silence_max_threshold ) {
        yyasr->hangover_hits = 0;
        yyasr->listen_hits++;
    } else {
        yyasr->hangover_hits++;
    }


    /* copy the sample data to the audio buffer
     * for recognition usage later (2 bytes for each sample) */
    // switch_log_printf(
    //     SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO,
    //     "hangover_hits=%d, avg_energy=%u, max_energy=%u\n", 
    //     yyasr->hangover_hits, avg_energy, max_energy
    // );
    if ( avg_energy > globals.feed_min_avg_energy 
        || max_energy > globals.feed_min_max_energy ) {
        /* resample the data */
        resamples = samples / 3;
        for ( c = 0; c < samples; c += 3 ) {
            one_sample = (data[c] + data[c+1] + data[c+2]) / 3;
            switch_buffer_write(yyasr->audio_buffer, &one_sample, sizeof(int16_t));
        }

        yyasr->audio_size += resamples * sizeof(int16_t);
        if ( yyasr->audio_size >= globals.audio_buffer_max_size ) {
            yyasr->audio_size = globals.audio_buffer_max_size;
            switch_set_flag_locked(yyasr, YY_FLAG_SPEECH_TIMEOUT);
            return SWITCH_TRUE;
        }
    }


    /* Check the silence timeout */
    if ( yyasr->hangover_hits > 0 && switch_test_flag(yyasr, YY_FLAG_INPUT_TIMERS) ) {
        if ( yyasr->listen_hits <= globals.min_listen_hits ) {
            if ( yyasr->hangover_hits >= globals.no_input_hangover 
                && ! switch_test_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT) ) {
                /*no input timeout*/
                switch_mutex_lock(yyasr->flag_mutex);
                switch_clear_flag(yyasr, YY_FLAG_READY);
                switch_set_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT);
                switch_mutex_unlock(yyasr->flag_mutex);
                // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "no input timeout\n");
                return SWITCH_TRUE;
            }
        } else if ( yyasr->hangover_hits >= globals.silence_hangover 
            && ! switch_test_flag(yyasr, YY_FLAG_SPEECH_TIMEOUT) ) {
            /*silence timeout*/
            switch_mutex_lock(yyasr->flag_mutex);
            switch_clear_flag(yyasr, YY_FLAG_READY);
            switch_set_flag(yyasr, YY_FLAG_SPEECH_TIMEOUT);
            switch_mutex_unlock(yyasr->flag_mutex);
            // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "silence timeout\n");
            return SWITCH_TRUE;
        }
    }

    return SWITCH_FALSE;
}





/*
 * function to open the asr interface 
 * invoke once when start the asr engine
*/
static switch_status_t yyasr_asr_open(
    switch_asr_handle_t *ah, 
    const char *codec, int rate, 
    const char *dest, switch_asr_flag_t *flags)
{
    yyasr_t *yyasr = switch_core_alloc(ah->memory_pool, sizeof(yyasr_t));
    if ( yyasr == NULL ) {
        return SWITCH_STATUS_MEMERR;
    }

    // ah->native_rate = 16000;
    ah->private_info = yyasr;
    yyasr->flags = 0;
    yyasr->pool = ah->memory_pool;

    switch_mutex_init(&yyasr->flag_mutex, SWITCH_MUTEX_NESTED, ah->memory_pool);
    yyasr->audio_size = 0;

    // initialized the audio_buffer and the text_buffer
    switch_buffer_create_dynamic(&yyasr->audio_buffer, 
        globals.audio_buffer_size, 
        globals.audio_buffer_size, globals.audio_buffer_max_size);
    switch_buffer_create_dynamic(&yyasr->text_buffer, 
        globals.text_buffer_size, 
        globals.text_buffer_size, globals.text_buffer_max_size);

    yyasr->hangover_hits = 0;
    yyasr->listen_hits = 0;

    switch_mutex_lock(yyasr->flag_mutex);
    switch_set_flag(yyasr, YY_FLAG_READY);
    switch_set_flag(yyasr, YY_FLAG_INPUT_TIMERS);
    switch_mutex_unlock(yyasr->flag_mutex);

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_open\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to close the asr interface */
static switch_status_t yyasr_asr_close(
    switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;
    if ( yyasr->audio_buffer ) {
        switch_buffer_destroy(&yyasr->audio_buffer);
    }

    if ( yyasr->text_buffer ) {
        switch_buffer_destroy(&yyasr->text_buffer);
    }

	switch_clear_flag(yyasr, YY_FLAG_READY);
	switch_set_flag(ah, SWITCH_ASR_FLAG_CLOSED);

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_close\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to feed audio to the ASR */
static switch_status_t yyasr_asr_feed(
    switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
    time_t time_ptr;
    int timestamp;
    char wave_file[WAVE_FILE_LENGTH];
    FILE *stream = NULL;

    yyasr_t *yyasr = (yyasr_t *) ah->private_info;

    // check the asr close flag
    if ( switch_test_flag(ah, SWITCH_ASR_FLAG_CLOSED) ) {
        return SWITCH_STATUS_BREAK;
    }

    if ( switch_test_flag(yyasr, YY_FLAG_READY) 
            && ! switch_test_flag(yyasr, YY_FLAG_RECOGNITION) ) {
        if ( stop_audio_detect(ah, (int16_t *) data, len / 2) 
            && yyasr->audio_size > 0 
                && ! switch_test_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT) ) {
            switch_set_flag_locked(yyasr, YY_FLAG_RECOGNITION);
            /* check and do the recognition api request*/
            // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
            //      "start recognition...\n");

            if ( globals.wav_file_dir != NULL && strlen(globals.wav_file_dir) < WAVE_DIR_MAX_LENGTH ) {
                timestamp = time(&time_ptr);
                memset(wave_file, 0x00, sizeof(wave_file));
                sprintf(wave_file, "%s/%d.pcm", globals.wav_file_dir, timestamp);
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "wave_file=%s\n", wave_file);
                stream = fopen(wave_file, "wb");
                if ( stream != NULL ) {
                    // write the PCM data
                    fwrite(
                        get_switch_buffer_ptr(yyasr->audio_buffer), 
                        switch_buffer_inuse(yyasr->audio_buffer), 1, stream
                    );
                    fclose(stream);
                }
            }


            switch_buffer_zero(yyasr->text_buffer);
            if ( recognition_by_rest_api(yyasr) ) {
                // @Note we also return the empty asr text
            }

            switch_mutex_lock(yyasr->flag_mutex);
            switch_clear_flag(yyasr, YY_FLAG_RECOGNITION);
            switch_set_flag(yyasr, YY_FLAG_HAS_TEXT);
            switch_mutex_unlock(yyasr->flag_mutex);
        } else if ( switch_test_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT) ) {
            /* never heard anything */
            switch_buffer_zero(yyasr->text_buffer);
            switch_mutex_lock(yyasr->flag_mutex);
            /* return a empty result */
            switch_set_flag(yyasr, YY_FLAG_HAS_TEXT);
            switch_clear_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT);
            switch_clear_flag(yyasr, YY_FLAG_READY);
            switch_mutex_unlock(yyasr->flag_mutex);
        }
    }

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_feed\n");
	return SWITCH_STATUS_SUCCESS;
}

/*! function to pause recognizer */
static switch_status_t yyasr_asr_pause(switch_asr_handle_t *ah)
{
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_mutex_lock(yyasr->flag_mutex);
	if ( switch_test_flag(yyasr, YY_FLAG_READY) ) {
		switch_clear_flag(yyasr, YY_FLAG_READY);
		status = SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_unlock(yyasr->flag_mutex);

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_pause\n");
    return status;
}

/*! function to resume recognizer */
static switch_status_t yyasr_asr_resume(switch_asr_handle_t *ah)
{
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;
	switch_status_t status = SWITCH_STATUS_FALSE;

	switch_mutex_lock(yyasr->flag_mutex);
	if ( ! switch_test_flag(yyasr, YY_FLAG_READY) ) {
        /*zero fill the audio_buffer and the audio size*/
        switch_buffer_zero(yyasr->audio_buffer);
        yyasr->audio_size = 0;
        yyasr->hangover_hits = 0;
        yyasr->listen_hits = 0;

        /*clear all the to stop flag*/
		switch_set_flag(yyasr, YY_FLAG_READY);
        switch_clear_flag(yyasr, YY_FLAG_SPEECH_TIMEOUT);
        switch_clear_flag(yyasr, YY_FLAG_NOINPUT_TIMEOUT);

		status = SWITCH_STATUS_SUCCESS;
	}
	switch_mutex_unlock(yyasr->flag_mutex);

    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_resume\n");
    return status;
}

/*! function to read results from the ASR*/
static switch_status_t yyasr_asr_check_results(
    switch_asr_handle_t *ah, switch_asr_flag_t *flags)
{
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;
    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "check_results\n");
    return switch_test_flag(yyasr, YY_FLAG_HAS_TEXT)
        ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

/*! function to read results from the ASR */
static switch_status_t yyasr_asr_get_results(switch_asr_handle_t *ah, char **xmlstr, switch_asr_flag_t *flags)
{
    yyasr_t *yyasr = (yyasr_t *) ah->private_info;
    
    if ( switch_test_flag(yyasr, YY_FLAG_HAS_TEXT) ) {
		*xmlstr = switch_mprintf("%s", get_switch_buffer_ptr(yyasr->text_buffer));
        // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, 
        //     "get_results.text=%s\n", *xmlstr);
        switch_clear_flag_locked(yyasr, YY_FLAG_HAS_TEXT);
        return SWITCH_STATUS_SUCCESS;
    }

    return SWITCH_STATUS_FALSE;
}

/*! function to start input timeouts */
static switch_status_t yyasr_asr_start_input_timers(switch_asr_handle_t *ah)
{
	yyasr_t *yyasr = (yyasr_t *) ah->private_info;
	switch_set_flag_locked(yyasr, YY_FLAG_INPUT_TIMERS);
    // switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "start_input_timers\n");
	return SWITCH_STATUS_SUCCESS;
}


/*! function to load a grammar to the asr interface */
static switch_status_t yyasr_asr_load_grammar(switch_asr_handle_t *ah, const char *grammar, const char *name)
{
    return SWITCH_STATUS_SUCCESS;
}

/*! function to unload a grammar to the asr interface */
static switch_status_t yyasr_asr_unload_grammar(switch_asr_handle_t *ah, const char *name)
{
	return SWITCH_STATUS_SUCCESS;
}

/*! set text parameter */
static void yyasr_asr_text_param(switch_asr_handle_t *ah, char *param, const char *val)
{
    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "asr_text_param\n");
}

/*! set numeric parameter */
static void yyasr_asr_numeric_param(switch_asr_handle_t *ah, char *param, int val)
{
}

/*! set float parameter */
static void yyasr_asr_float_param(switch_asr_handle_t *ah, char *param, double val)
{
}



static switch_status_t yyasr_load_config(void)
{
	char *cf = "yyasr.conf";
    size_t url_len = 0;
    char *api_base;
	switch_xml_t cfg, xml = NULL, param, settings;
	switch_status_t status = SWITCH_STATUS_SUCCESS;

	/* default the settings*/
	globals.app_key = "";
	globals.engine = "aispeech";
	globals.model  = "comm";
	globals.vad = SWITCH_TRUE;
	globals.nlu = SWITCH_TRUE;

	globals.silence_avg_threshold = 128;
    globals.silence_max_threshold = 540;
    globals.feed_min_avg_energy = 30;
    globals.feed_min_max_energy = 164;
    globals.no_input_hangover = 400;
    globals.silence_hangover = 25;
    globals.min_listen_hits = 10;

	globals.audio_buffer_size = 1024 * 32;
	globals.audio_buffer_max_size = 1024 * 1024 * 2;
	globals.text_buffer_size = 48;
	globals.text_buffer_max_size = 1024 * 2;
    globals.nlu_buffer_size = 1024 * 30;
    globals.nlu_buffer_max_size = 1024 * 100;
    globals.wav_file_dir = NULL;

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "load config\n");
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
			} else if ( strcasecmp(var, "engine") == 0 ) {
				globals.engine = switch_core_strdup(globals.pool, val);
			} else if ( strcasecmp(var, "model") == 0 ) {
				globals.model = switch_core_strdup(globals.pool, val);
			} else if ( strcasecmp(var, "vad") == 0 ) {
				globals.vad = switch_true(val);
			} else if ( strcasecmp(var, "nlu") == 0 ) {
				globals.nlu = switch_true(val);
			} else if ( strcasecmp(var, "silence-avg-threshold") == 0 ) {
				globals.silence_avg_threshold = atoi(val);
            } else if ( strcasecmp(var, "silence-max-threshold") == 0 ) {
                globals.silence_max_threshold = atoi(val);
            } else if ( strcasecmp(var, "feed-min-avg-energy") == 0 ) {
                globals.feed_min_avg_energy = atoi(val);
            } else if ( strcasecmp(var, "feed-min-max-energy") == 0 ) {
                globals.feed_min_max_energy = atoi(val);
            } else if ( strcasecmp(var, "silence-hangover") == 0 ) {
                globals.silence_hangover = atoi(val);
            } else if ( strcasecmp(var, "no-input-hangover") == 0 ) {
                globals.no_input_hangover = atoi(val);
            } else if ( strcasecmp(var, "min-listen-hits") == 0 ) {
                globals.min_listen_hits = atoi(val);
            } else if ( strcasecmp(var, "audio-buffer-size") == 0 ) {
                globals.audio_buffer_size = atoi(val);
            } else if ( strcasecmp(var, "audio-buffer-max-size") == 0 ) {
                globals.audio_buffer_max_size = atoi(val);
            } else if ( strcasecmp(var, "text-buffer-size") == 0 ) {
                globals.text_buffer_size = atoi(val);
            } else if ( strcasecmp(var, "text-buffer-max-size") == 0 ) {
                globals.text_buffer_max_size = atoi(val);
            } else if ( strcasecmp(var, "wav-file-dir") == 0 ) {
                globals.wav_file_dir = switch_core_strdup(globals.pool, val);
            } else if ( strcasecmp(var, "auto-reload") == 0 ) {
                globals.auto_reload = switch_true(val);
            } 
		}
	}

  done:
	if (xml) {
		switch_xml_free(xml);
	}


    /* Make the final asr or the nlu api url */
    if ( globals.nlu == SWITCH_TRUE ) {
        // @Length length for ?app_key=
        api_base = "http://sebiengine.ai/api/semantics/";
        url_len  = strlen(api_base);
        url_len += 9;                           // @Length
        url_len += strlen(globals.app_key);     // bytes for app_key

        globals.api_url = (char *) switch_core_alloc(globals.pool, url_len + 1);
        strcat(globals.api_url, api_base);
        strcat(globals.api_url, "?app_key=");
        strcat(globals.api_url, globals.app_key);
    } else {
        // @Length length for ?app_key=&engine=&model=&vad=false&user_id=fs_sdk_9 
        api_base = "http://sebiengine.ai/api/acoustics/asr/";
        url_len  = strlen(api_base);
        url_len += 51;                          // @Length
        url_len += strlen(globals.engine);
        url_len += strlen(globals.model);
        url_len += strlen(globals.app_key);     // bytes for app_key

        globals.api_url = (char *) switch_core_alloc(globals.pool, url_len + 1);
        strcat(globals.api_url, api_base);
        strcat(globals.api_url, "?app_key=");
        strcat(globals.api_url, globals.app_key);
        strcat(globals.api_url, "&engine=");
        strcat(globals.api_url, globals.engine);
        strcat(globals.api_url, "&model=");
        strcat(globals.api_url, globals.model);
        strcat(globals.api_url, "&vad=");
        strcat(globals.api_url, globals.vad ? "true" : "false");
        strcat(globals.api_url, "&user_id=fs_sdk_9");
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "yyasr.api_url=%s\n", globals.api_url);

	return status;
}

static void do_config_load(void)
{
	switch_mutex_lock(MUTEX);
	yyasr_load_config();
	switch_mutex_unlock(MUTEX);
}

static void event_handler(switch_event_t *event)
{
	if ( globals.auto_reload ) {
		do_config_load();
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_INFO, "yyasr reloaded\n");
	}
}


SWITCH_MODULE_LOAD_FUNCTION(mod_yyasr_load)
{
	switch_asr_interface_t *asr_interface;

    switch_mutex_init(&MUTEX, SWITCH_MUTEX_NESTED, pool);
    globals.pool = pool;

	if ((switch_event_bind_removable(modname, SWITCH_EVENT_RELOADXML, NULL, event_handler, NULL, &NODE) != SWITCH_STATUS_SUCCESS)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't bind!\n");
	}

	do_config_load();

	/* connect my internal structure to the blank pointer passed to me */
	*module_interface = switch_loadable_module_create_module_interface(pool, modname);

	asr_interface = switch_loadable_module_create_interface(*module_interface, SWITCH_ASR_INTERFACE);
	asr_interface->interface_name = "yyasr";
	asr_interface->asr_open = yyasr_asr_open;
	asr_interface->asr_load_grammar = yyasr_asr_load_grammar;
	asr_interface->asr_unload_grammar = yyasr_asr_unload_grammar;
	asr_interface->asr_close = yyasr_asr_close;
	asr_interface->asr_feed = yyasr_asr_feed;
	asr_interface->asr_resume = yyasr_asr_resume;
	asr_interface->asr_pause = yyasr_asr_pause;
	asr_interface->asr_check_results = yyasr_asr_check_results;
	asr_interface->asr_get_results = yyasr_asr_get_results;
	asr_interface->asr_start_input_timers = yyasr_asr_start_input_timers;
	asr_interface->asr_text_param = yyasr_asr_text_param;
	asr_interface->asr_numeric_param = yyasr_asr_numeric_param;
	asr_interface->asr_float_param = yyasr_asr_float_param;

	/* indicate that the module should continue to be loaded */
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_yyasr_shutdown)
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
