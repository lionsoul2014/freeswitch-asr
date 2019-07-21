## Description
A TTS module and a ASR module with auto Voice Active Detect supported for Freeswitch. 
I build it for Nature sound interactive, With the embedded LUA engine we could easly build a Freeswtich application like this.


## How to use it ?
1. Copy and merge the ${root}/src and the ${root}/conf with the Freeswitch source tree.
2. Add the following two module in the ${FREESWITCH_SOURCE_ROOT}/modules.conf
```
asr_tts/mod_yytts
asr_tts/mod_yyasr
```

3. Re-compile and install the freeswitch to install mod_yytts and mod_yyasr modules.
4. Active the mod_yytts and the mod_yyasr by add the following to lines into the ${FREESWITCH_INSTALLATION_ROOT}/conf/autoload_configs/modules.conf.xml
```
<load module="mod_yytts"/>
<load module="mod_yyasr"/>
```

5. Copy the lua scripts under ${root}/scripts to ${FREESWITCH_INSTALLATION_ROOT}/scripts/
6. Bind a number to build application by adding the following xml settings to the ${FREESWITCH_INSTALLATION_ROOT}/conf/dialplan/default.xml
```
<!-- yytts tts module testing -->
<extension name="yytts_demo">
  <condition field="destination_number" expression="^5001$">
    <action application="answer"/>
    <action application="sleep" data="2000"/>
    <action application="lua" data="yytts_demo.lua"/>
  </condition>
</extension>

<!-- yyasr asr module testing -->
<extension name="yyasr_demo">
  <condition field="destination_number" expression="^5002$">
    <action application="answer"/>
    <action application="sleep" data="2000"/>
    <action application="lua" data="yyasr_demo.lua"/>
  </condition>
</extension>
``` 

## How to test it ?
Start the Freeswitch and install a Linphone client.
Dial 5001 for TTS testing and 5003 for ASR testing.


## Secondary development

#### TTS module
1. TTS module structure:
```c
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
```

2. Invoke process for TTS module in Freeswitch:
```
open
while ( thread running ) do
	feed
	read
	flush
end
close
```


3. Embedded the other TTS engine by override the yytts_speech_feed_tts
```c
static switch_status_t yytts_speech_feed_tts(switch_speech_handle_t *sh, char *text, switch_speech_flag_t *flags)
{
	// feed the wave stream to the yytts->audio_buffer buffer
	return SWITCH_STATUS_SUCCESS;
}
```

#### ASR module
1. ASR module structure:
```
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
```

2. Invoke process for ASR module in Freeswitch:
```
open
while ( thread running ) do
	feed
	check_results
	get_results
end
optional pause|resume
close
```

3. Embedded the other ASR engine by override the yyasr_asr_feed
```c
/*! function to feed audio to the ASR */
static switch_status_t yyasr_asr_feed(
    switch_asr_handle_t *ah, void *data, unsigned int len, switch_asr_flag_t *flags)
{
	// feed the text to the yyasr->text_buffer text buffer
	return SWITCH_STATUS_SUCCESS;
}


/*
 * Voice Active detecting implementation.
 * 
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
```
