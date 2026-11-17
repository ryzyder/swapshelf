/* Prefijo de símbolos para enlazar varios núcleos libretro en SwapShelf.
 * Cada núcleo exporta la misma API retro_*. Al compilarlos estáticamente en
 * un único ejecutable, SWS_RETRO_PREFIX evita que sus símbolos choquen. */
#ifndef SWS_RETRO_PREFIX
#error "Define SWS_RETRO_PREFIX al compilar un núcleo libretro"
#endif

#define SWS_JOIN_RAW(a, b) a##b
#define SWS_JOIN(a, b) SWS_JOIN_RAW(a, b)
#define SWS_RETRO(name) SWS_JOIN(SWS_RETRO_PREFIX, name)

#define retro_api_version SWS_RETRO(retro_api_version)
#define retro_cheat_reset SWS_RETRO(retro_cheat_reset)
#define retro_cheat_set SWS_RETRO(retro_cheat_set)
#define retro_deinit SWS_RETRO(retro_deinit)
#define retro_get_memory_data SWS_RETRO(retro_get_memory_data)
#define retro_get_memory_size SWS_RETRO(retro_get_memory_size)
#define retro_get_region SWS_RETRO(retro_get_region)
#define retro_get_system_av_info SWS_RETRO(retro_get_system_av_info)
#define retro_get_system_info SWS_RETRO(retro_get_system_info)
#define retro_init SWS_RETRO(retro_init)
#define retro_load_game SWS_RETRO(retro_load_game)
#define retro_load_game_special SWS_RETRO(retro_load_game_special)
#define retro_reset SWS_RETRO(retro_reset)
#define retro_run SWS_RETRO(retro_run)
#define retro_serialize SWS_RETRO(retro_serialize)
#define retro_serialize_size SWS_RETRO(retro_serialize_size)
#define retro_set_audio_sample SWS_RETRO(retro_set_audio_sample)
#define retro_set_audio_sample_batch SWS_RETRO(retro_set_audio_sample_batch)
#define retro_set_controller_port_device SWS_RETRO(retro_set_controller_port_device)
#define retro_set_environment SWS_RETRO(retro_set_environment)
#define retro_set_input_poll SWS_RETRO(retro_set_input_poll)
#define retro_set_input_state SWS_RETRO(retro_set_input_state)
#define retro_set_video_refresh SWS_RETRO(retro_set_video_refresh)
#define retro_unload_game SWS_RETRO(retro_unload_game)
#define retro_unserialize SWS_RETRO(retro_unserialize)
