void multistream_mapper_init(uint32_t n_soft_clusters, uint32_t n_hard_clusters);
double calc_compress_ratio(void *mb, uint64_t lpn);
uint32_t get_soft_sid(double compress_ratio);
uint32_t get_hard_sid(uint64_t lifetime);
