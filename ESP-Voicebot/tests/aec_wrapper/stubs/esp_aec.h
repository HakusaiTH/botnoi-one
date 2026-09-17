#pragma once
#include <stdint.h>

enum aec_mode_t { AEC_MODE_FD_LOW_COST = 5 };
enum aec_nlp_level_t { AEC_NLP_LEVEL_NORMAL = 0 };
struct aec_config_t {
  int mic_num, ref_num, out_num, filter_length, sample_rate;
  uint32_t caps;
  aec_mode_t mode;
  aec_nlp_level_t nlp_level;
};
struct aec_handle_t {
  void* aec_handle;
  int frame_size;
  aec_config_t config;
};
aec_handle_t* aec_create_from_config(aec_config_t*);
int aec_get_chunksize(const aec_handle_t*);
void aec_process(const aec_handle_t*, int16_t*, int16_t*, int16_t*);
void aec_destroy(aec_handle_t*);
