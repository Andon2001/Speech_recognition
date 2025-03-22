typedef struct{
    int32_t cw;    /**< Codeword index. */
    int32_t score; /**< Score. */
}ptm_topn_t;

typedef struct
{
  ptm_topn_t topn[32];
  int16_t max;
  int32_t mean[2048];
  int32_t var[2048];
  int32_t det[256];
  int32_t density;
  int32_t featlen;
  int32_t z[32];
  bool ready;
}speech_struct;
