//
// Created by 付聪 on 2017/6/21.
//

#include <chrono>

#include "index_random.h"
#include "index_ssg.h"
#include "util.h"
#include <omp.h>

void save_result(char* filename, std::vector<std::vector<unsigned> >& results) {
  std::ofstream out(filename, std::ios::binary | std::ios::out);

  for (unsigned i = 0; i < results.size(); i++) {
    unsigned GK = (unsigned)results[i].size();
    out.write((char*)&GK, sizeof(unsigned));
    out.write((char*)results[i].data(), GK * sizeof(unsigned));
  }
  out.close();
}

int main(int argc, char** argv) {
  if (argc < 11) {
    std::cout << "./run data_file query_file ssg_path L K result_path ground_truth_path hash_bitwidth threshold_percent num_threads [seed]"
              << std::endl;
    exit(-1);
  }

  if (argc == 12) {
    unsigned seed = (unsigned)atoi(argv[11]);
    srand(seed);
    std::cerr << "Using Seed " << seed << std::endl;
  }

  std::cerr << "Data Path: " << argv[1] << std::endl;

  unsigned points_num, dim;
  float* data_load = nullptr;
  data_load = efanna2e::load_data(argv[1], points_num, dim);
  data_load = efanna2e::data_align(data_load, points_num, dim);

  std::cerr << "Query Path: " << argv[2] << std::endl;

  unsigned query_num, query_dim;
  float* query_load = nullptr;
  query_load = efanna2e::load_data(argv[2], query_num, query_dim);
  query_load = efanna2e::data_align(query_load, query_num, query_dim);

  assert(dim == query_dim);

  efanna2e::IndexRandom init_index(dim, points_num);
  efanna2e::IndexSSG index(dim, points_num, efanna2e::FAST_L2,
                           (efanna2e::Index*)(&init_index));

  std::cerr << "SSG Path: " << argv[3] << std::endl;
  std::cerr << "Result Path: " << argv[6] << std::endl;

#ifdef THETA_GUIDED_SEARCH
  index.hash_bitwidth = (unsigned)atoi(argv[8]);
  index.threshold_percent = (float)atof(argv[9]);
#endif

  index.Load(argv[3]);
  index.OptimizeGraph(data_load);

#ifdef THETA_GUIDED_SEARCH
  // SJ: For profile, related with #THETA_GUIDED_SEARCH flag
  char* hash_function_name = new char[strlen(argv[3]) + strlen(".hash_function_") + strlen(argv[9]) + 1];
  char* hash_vector_name = new char[strlen(argv[3]) + strlen(".hash_vector") + strlen(argv[9]) + 1];
  strcpy(hash_function_name, argv[3]);
  strcat(hash_function_name, ".hash_function_");
  strcat(hash_function_name, argv[8]);
  strcat(hash_function_name, "b");
  strcpy(hash_vector_name, argv[3]);
  strcat(hash_vector_name, ".hash_vector_");
  strcat(hash_vector_name, argv[8]);
  strcat(hash_vector_name, "b");

  if (index.LoadHashFunction(hash_function_name)) {
    if (!index.LoadHashValue(hash_vector_name))
      index.GenerateHashValue(hash_vector_name);
  }
  else {
    index.GenerateHashFunction(hash_function_name);
    index.GenerateHashValue(hash_vector_name);
  }
#endif

  unsigned L = (unsigned)atoi(argv[4]);
  unsigned K = (unsigned)atoi(argv[5]);

  std::cerr << "L = " << L << ", ";
  std::cerr << "K = " << K << std::endl;

  efanna2e::Parameters paras;
  paras.Set<unsigned>("L_search", L);

  std::vector<std::vector<unsigned> > res(query_num);
  for (unsigned i = 0; i < query_num; i++) res[i].resize(K);

  unsigned int num_threads = atoi(argv[10]);
#ifdef THREAD_LATENCY
  std::vector<double> latency_stats(query_num, 0);
#endif
#ifdef PROFILE
  index.num_timer = 3;
  index.profile_time.resize(num_threads * index.num_timer, 0.0);
#endif
  boost::dynamic_bitset<> flags{index.get_nd(), 0};
  // Warm up
  for (int loop = 0; loop < 3; ++loop) {
    for (unsigned i = 0; i < 10; ++i) {
      index.SearchWithOptGraph(query_load + i * dim, flags, K, paras, res[i].data());
    }
  }

  omp_set_num_threads(num_threads);
  auto s = std::chrono::high_resolution_clock::now();
//#pragma omp parallel for schedule(dynamic, 10)
  for (unsigned i = 0; i < query_num; i++) {
#ifdef THREAD_LATENCY
    auto query_start = std::chrono::high_resolution_clock::now();
#endif
    index.SearchWithOptGraph(query_load + i * dim, flags, K, paras, res[i].data());
#ifdef THREAD_LATENCY
   auto query_end = std::chrono::high_resolution_clock::now();
   std::chrono::duration<double> query_diff = query_end - query_start;
   latency_stats[i] = query_diff.count() * 1000000;
#endif
  }
  auto e = std::chrono::high_resolution_clock::now();

  std::chrono::duration<double> diff = e - s;
  std::cerr << "Search Time: " << diff.count() << std::endl;
#ifdef GET_MISS_TRAVERSE
  std::cerr << std::endl;
  std::cerr << "[Total_summary] # of traversed: "<< index.total_traverse <<", # of invalid: "<< index.total_traverse_miss << ", ratio: "<< (float)index.total_traverse_miss / index.total_traverse * 100<<std::endl;
  //printf("[Total_summary] # of traversed: %u, # of invalid: %u, ratio: %.2f%%\n", index.total_traverse, index.total_traverse_miss, (float)index.total_traverse_miss / index.total_traverse * 100);
#endif
#ifdef EVAL_RECALL
  std::cerr << "QPS: " << query_num / diff.count() << std::endl;
  unsigned int* ground_truth_load = NULL;
  unsigned ground_truth_num, ground_truth_dim;
  ground_truth_load = efanna2e::load_data_ivecs(argv[7], ground_truth_num, ground_truth_dim);
  
  unsigned int topk_hit = 0;
  for (unsigned int i = 0; i < query_num; i++) {
    unsigned int topk_local_hit = 0;
    for (unsigned int j = 0; j < K; j++) {
      for (unsigned int k = 0; k < K; k++) {
        if (res[i][j] == *(ground_truth_load + i * ground_truth_dim + k)) {
          topk_hit++;
          break;
        }
      }
    }
  }
  std::cerr << (float)topk_hit / (query_num * K) * 100 << "%" << std::endl;
#endif
#ifdef THREAD_LATENCY
  std::sort(latency_stats.begin(), latency_stats.end());
  double mean_latency = 0;
  for (uint64_t q = 0; q < query_num; q++) {
    mean_latency += latency_stats[q];
  }
  mean_latency /= query_num;
  std::cerr << "mean_latency: " << mean_latency << "ms" << std::endl;
  std::cerr << "99% latency: " << latency_stats[(unsigned long long)(0.999 * query_num)] << "ms"<< std::endl;
#endif
#ifdef PROFILE
  std::cerr << "========Thread Latency Report========" << std::endl;
  double* timer = (double*)calloc(index.num_timer, sizeof(double));
  for (unsigned int tid = 0; tid < num_threads; tid++) {
    timer[0] += index.profile_time[tid * index.num_timer];
    timer[1] += index.profile_time[tid * index.num_timer + 1];
    timer[2] += index.profile_time[tid * index.num_timer + 2];
  }
#ifdef THETA_GUIDED_SEARCH
  std::cerr << "query_hash time: " << timer[0] / query_num << "ms" << std::endl;
  std::cerr << "hash_approx time: " << timer[1] / query_num << "ms" << std::endl;
  std::cerr << "dist time: " << timer[2] / query_num << "ms" << std::endl;
#else
  std::cerr << "dist time: " << timer[2] / query_num << "ms" << std::endl;
#endif
  std::cerr << "=====================================" << std::endl;
#endif
#ifdef THETA_GUIDED_SEARCH
  delete[] hash_function_name;
  delete[] hash_vector_name;
#endif

  save_result(argv[6], res);

  return 0;
}
