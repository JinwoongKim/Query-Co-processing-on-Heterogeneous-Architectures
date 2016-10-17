#include "tree/rtree_ls.h"

#include "common/macro.h"
#include "common/logger.h"
#include "evaluator/recorder.h"
#include "sort/sorter.h"
#include "transformer/transformer.h"
#include "manager/chunk_manager.h"

#include <cassert>
#include <thread>
#include <algorithm>
#include <chrono> // for sleep

#include "cuda_profiler_api.h"

namespace ursus {
namespace tree {

RTree_LS::RTree_LS() { 
  tree_type = TREE_TYPE_RTREE_LS;
}

/**
 * @brief build trees on the GPU
 * @param input_data_set 
 * @return true if success to build otherwise false
 */
bool RTree_LS::Build(std::shared_ptr<io::DataSet> input_data_set) {

  LOG_INFO("Build RTree_LS Tree");
  bool ret = false;

  // Load an index from file it exists
  // otherwise, build an index and dump it to file
  auto index_name = GetIndexName(input_data_set);
  if(input_data_set->IsRebuild() || !DumpFromFile(index_name)) {
    //===--------------------------------------------------------------------===//
    // Create branches
    //===--------------------------------------------------------------------===//
    std::vector<node::Branch> branches = CreateBranches(input_data_set);

LOG_INFO("");
    ret = RTree_LS_Top_Down(branches); 
    assert(ret);

    //===--------------------------------------------------------------------===//
    // Transform nodes into SOA fashion 
    //===--------------------------------------------------------------------===//
    
LOG_INFO("");
    node_soa_ptr = transformer::Transformer::Transform(b_node_ptr, GetNumberOfNodeSOA());
    assert(node_soa_ptr);

    delete b_node_ptr;

    // Dump an index to the file
    DumpToFile(index_name);
  } 

LOG_INFO("");
  //===--------------------------------------------------------------------===//
  // Move Trees to the GPU in advance
  //===--------------------------------------------------------------------===//
  auto& chunk_manager = manager::ChunkManager::GetInstance();

  ui offset = 0;
  ui count = 0;

  assert(scan_level <= level_node_count.size());

  for(ui range(i, 0, level_node_count.size()-scan_level)) {
    offset += level_node_count[i];
  }
  count = GetNumberOfNodeSOA()-offset;

  // Get Chunk Manager and initialize it
  chunk_manager.Init(sizeof(node::Node_SOA)*count);
  chunk_manager.CopyNode(node_soa_ptr+offset, 0, count);

  return true;
}

void RTree_LS::Thread_BuildExtendLeafNodeOnCPU(ul current_offset, ul parent_offset, 
                                             ui number_of_node, ui tid, ui number_of_threads) {
  node::Node_SOA* current_node;
  node::Node_SOA* parent_node;

  for(ui range(node_offset, tid, number_of_node, number_of_threads)) {
    current_node = node_soa_ptr+current_offset+node_offset;
    parent_node = node_soa_ptr+parent_offset+(ul)(node_offset/GetNumberOfLeafNodeDegrees());

    parent_node->SetChildOffset(node_offset%GetNumberOfLeafNodeDegrees(), 
                                (ll)current_node-(ll)parent_node);

    parent_node->SetIndex(node_offset%GetNumberOfLeafNodeDegrees(), current_node->GetLastIndex());

    parent_node->SetLevel(current_node->GetLevel()-1);
    parent_node->SetBranchCount(GetNumberOfLeafNodeDegrees());

    // Set the node type
    if(current_node->GetNodeType() == NODE_TYPE_LEAF) {
      parent_node->SetNodeType(NODE_TYPE_EXTENDLEAF);
    } else {
      parent_node->SetNodeType(NODE_TYPE_INTERNAL); 
    }

    //Find out the min, max boundaries in this node and set up the parent rect.
    for(ui range(dim, 0, GetNumberOfDims())) {
      ui high_dim = dim+GetNumberOfDims();

      float lower_boundary[GetNumberOfLeafNodeDegrees()];
      float upper_boundary[GetNumberOfLeafNodeDegrees()];

      for( ui range(thread_itr, 0, GetNumberOfLeafNodeDegrees())) {
        if( thread_itr < current_node->GetBranchCount()){
          lower_boundary[ thread_itr ] = current_node->GetBranchPoint(thread_itr, dim);
          upper_boundary[ thread_itr ] = current_node->GetBranchPoint(thread_itr, high_dim);
        } else {
          lower_boundary[ thread_itr ] = 1.0f;
          upper_boundary[ thread_itr ] = 0.0f;
        }
      }

      FindMinOnCPU(lower_boundary, GetNumberOfLeafNodeDegrees());
      FindMaxOnCPU(upper_boundary, GetNumberOfLeafNodeDegrees());

      parent_node->SetBranchPoint( (node_offset % GetNumberOfLeafNodeDegrees()), lower_boundary[0], dim);
      parent_node->SetBranchPoint( (node_offset % GetNumberOfLeafNodeDegrees()), upper_boundary[0], high_dim);
    }
  }

  //last node in each level
  if(  number_of_node % GetNumberOfLeafNodeDegrees() ){
    parent_node = node_soa_ptr + current_offset - 1;
    if( number_of_node < GetNumberOfLeafNodeDegrees() ) {
      parent_node->SetBranchCount(number_of_node);
    }else{
      parent_node->SetBranchCount(number_of_node%GetNumberOfLeafNodeDegrees());
    }
  }
}

bool RTree_LS::BuildExtendLeafNodeOnCPU() {

  const size_t number_of_threads = std::thread::hardware_concurrency();

  // parallel for loop using c++ std 11 
  {
    std::vector<std::thread> threads;
    ul current_offset = GetNumberOfExtendLeafNodeSOA();
    ul parent_offset = 0;

    for (ui range(thread_itr, 0, number_of_threads)) {
      threads.push_back(std::thread(&RTree_LS::Thread_BuildExtendLeafNodeOnCPU, 
                        this, current_offset, parent_offset, 
                        GetNumberOfLeafNodeSOA(), thread_itr, number_of_threads));
    }
    //Join the threads with the main thread
    for(auto &thread : threads){
      thread.join();
    }
    threads.clear();
  }

  return true;
}

bool RTree_LS::DumpFromFile(std::string index_name) {
  auto& recorder = evaluator::Recorder::GetInstance();
  recorder.TimeRecordStart();

  // naming
  std::string upper_tree_name = index_name;
  std::string flat_array_name = index_name;
  auto pos = upper_tree_name.find("RTREE_LS");

  switch(UPPER_TREE_TYPE){
// Disable BVH
//    case TREE_TYPE_BVH:{
//      upper_tree_name.replace(pos, 6, "BVH");
//      flat_array_name.replace(pos, 6, "HYBRIDBVH");
//      }break;
    case TREE_TYPE_RTREE:{
      upper_tree_name.replace(pos, 6, "RTREE_LS_INT"); 
      flat_array_name.replace(pos, 6, "RTREE_LS_LEAF");
      }break;
    default:
      LOG_INFO("Something's wrong!");
      break;
  }


  FILE* upper_tree_index_file=nullptr;
  FILE* flat_array_index_file=nullptr;

  // check file exists
  if(IsExist(upper_tree_name)){
    upper_tree_exists = true;
    upper_tree_index_file = OpenIndexFile(upper_tree_name);
  }
  if(IsExist(flat_array_name)){
    flat_array_exists = true;
    flat_array_index_file = OpenIndexFile(flat_array_name);
  }


  //===--------------------------------------------------------------------===//
  // Node counts
  //===--------------------------------------------------------------------===//
  // read host node count
  if(upper_tree_exists){
    fread(&host_node_count, sizeof(ui), 1, upper_tree_index_file);
  }

  // read device count for GPU
  if(flat_array_exists){
    fread(&device_node_count, sizeof(ui), 1, flat_array_index_file);

    ui height;
    fread(&height, sizeof(ui), 1, flat_array_index_file);
    level_node_count.resize(height);

    for(ui range(i, 0, height)){
      fread(&level_node_count[i], sizeof(ui), 1, flat_array_index_file);
    }
  }

  //===--------------------------------------------------------------------===//
  // Nodes for CPU
  //===--------------------------------------------------------------------===//
  if(upper_tree_exists) {
    node_ptr = new node::Node[host_node_count];
    fread(node_ptr, sizeof(node::Node), host_node_count, upper_tree_index_file);
  }

  //===--------------------------------------------------------------------===//
  // Nodes for GPU
  //===--------------------------------------------------------------------===//
  if(flat_array_exists){
    node_soa_ptr = new node::Node_SOA[device_node_count];
    fread(node_soa_ptr, sizeof(node::Node_SOA), device_node_count, flat_array_index_file);
  }

  auto elapsed_time = recorder.TimeRecordEnd();

  if(upper_tree_index_file) {
    LOG_INFO("DumpFromFile %s", upper_tree_name.c_str());
    LOG_INFO("Done, time = %.6fs", elapsed_time/1000.0f);
    fclose(upper_tree_index_file);
  }
  if(flat_array_index_file) {
    LOG_INFO("DumpFromFile %s", flat_array_name.c_str());
    LOG_INFO("Done, time = %.6fs", elapsed_time/1000.0f);
    fclose(flat_array_index_file);
  }

  // return true all of them exist
  if(upper_tree_exists && flat_array_exists){
    return true;
  }
  // otherwise, dump an index to file what we create now

  return false;
}

bool RTree_LS::DumpToFile(std::string index_name) {
  auto& recorder = evaluator::Recorder::GetInstance();
  recorder.TimeRecordStart();

  // naming
  std::string upper_tree_name = index_name;
  std::string flat_array_name = index_name;
  auto pos = upper_tree_name.find("RTREE_LS");

  switch(UPPER_TREE_TYPE){
//    case TREE_TYPE_BVH:{
//      upper_tree_name.replace(pos, 6, "BVH");
//      flat_array_name.replace(pos, 6, "HYBRIDBVH");
//      } break;
    case TREE_TYPE_RTREE:{
      upper_tree_name.replace(pos, 6, "RTREE_LS_INT"); 
      flat_array_name.replace(pos, 6, "RTREE_LS_LEAF");
      } break;
    default:
      LOG_INFO("Something's wrong!");
      break;
  }

  // NOTE :: Use fwrite as it is fast

  FILE* upper_tree_index_file;
  FILE* flat_array_index_file;
  if(!upper_tree_exists){
    upper_tree_index_file = fopen(upper_tree_name.c_str(),"wb");
  }
  if(!flat_array_exists){
    flat_array_index_file = fopen(flat_array_name.c_str(),"wb");
  }

  //===--------------------------------------------------------------------===//
  // Node counts
  //===--------------------------------------------------------------------===//
  // write node count for CPU
  if(!upper_tree_exists){
    fwrite(&host_node_count, sizeof(ui), 1, upper_tree_index_file);
  }

  // write node count for GPU
  if(!flat_array_exists){
    fwrite(&device_node_count, sizeof(ui), 1, flat_array_index_file);

    ui height = level_node_count.size();
    fwrite(&height, sizeof(ui), 1, flat_array_index_file);

    for(auto node_count : level_node_count){
      fwrite(&node_count, sizeof(ui), 1, flat_array_index_file);
    }
  }

  //===--------------------------------------------------------------------===//
  // Internal nodes
  //===--------------------------------------------------------------------===//

  // Unlike dump function in MPHR class, we use the queue structure to dump the
  // tree onto an index file since the nodes are allocated here and there in a
  // Top-Down fashion
  if(!upper_tree_exists){
    std::queue<node::Node*> bfs_queue;
    std::vector<ll> original_child_offset; // for backup

    // push the root node
    bfs_queue.emplace(node_ptr);

    // if the queue is not empty,
    while(!bfs_queue.empty()) {
      // pop the first element 
      node::Node* node = bfs_queue.front();
      bfs_queue.pop();

      // NOTE : Backup the child offsets in order to recover node's child offset later
      // I believe accessing memory is faster than accesing disk,
      // I don't use another fwrite for this job.
      if( node->GetNodeType() == NODE_TYPE_INTERNAL) {
        for(ui range(child_itr, 0, node->GetBranchCount())) {
          node::Node* child_node = node->GetBranchChildNode(child_itr);
          bfs_queue.emplace(child_node);

          // backup current child offset
          original_child_offset.emplace_back(node->GetBranchChildOffset(child_itr));

          // reassign child offset
          ll child_offset = (ll)bfs_queue.size()*(ll)sizeof(node::Node);
          node->SetBranchChildOffset(child_itr, child_offset);
        }
      }

      // write an internal node on disk
      fwrite(node, sizeof(node::Node), 1, upper_tree_index_file);

      // Recover child offset
      if( node->GetNodeType() == NODE_TYPE_INTERNAL) {
        for(ui range(child_itr, 0, node->GetBranchCount())) {
          // reassign child offset
          node->SetBranchChildOffset(child_itr, original_child_offset[child_itr]);
        }
      }
      original_child_offset.clear();
    }
  }

  //===--------------------------------------------------------------------===//
  // Extend & leaf nodes
  //===--------------------------------------------------------------------===//
  if(!flat_array_exists){
    fwrite(node_soa_ptr, sizeof(node::Node_SOA), GetNumberOfNodeSOA(), flat_array_index_file);
  }


  if(upper_tree_index_file){
    LOG_INFO("DumpToFile %s", upper_tree_name.c_str());
    fclose(upper_tree_index_file);
  }
  if(flat_array_index_file){
    LOG_INFO("DumpToFile %s", flat_array_name.c_str());
    fclose(flat_array_index_file);
  }

  auto elapsed_time = recorder.TimeRecordEnd();
  LOG_INFO("Done, time = %.6fs", elapsed_time/1000.0f);
  return true;
}

ui RTree_LS::GetNumberOfNodeSOA() const{
  assert(device_node_count);
  return device_node_count;
}

ui RTree_LS::GetNumberOfLeafNodeSOA() const {
  assert(level_node_count.back());
  return level_node_count.back();
}

ui RTree_LS::GetNumberOfExtendLeafNodeSOA() const {
  auto height = level_node_count.size();
  assert(height>1);
  return level_node_count[level_node_count.size()-2];;
}

int RTree_LS::Search(std::shared_ptr<io::DataSet> query_data_set, 
                   ui number_of_search, ui number_of_repeat){

  auto& recorder = evaluator::Recorder::GetInstance();

  //===--------------------------------------------------------------------===//
  // Read Query 
  //===--------------------------------------------------------------------===//
  auto query = query_data_set->GetPoints();
  auto d_query = query_data_set->GetDeviceQuery(number_of_search);

  //===--------------------------------------------------------------------===//
  // Set # of threads and Chunk Size
  //===--------------------------------------------------------------------===//
  ui number_of_blocks_per_cpu = GetNumberOfBlocks()/number_of_cpu_threads;
  // chunk size should be equal or larger than number of blocks per cpu
  // otherwise, just wasting GPU resources.
  // NOTE :: If scan level is 2 and chunk size is 1, we actually scan 128 leaf nodes.
  {
    ui _chunk_size = chunk_size;
    for(ui range(i, 1, scan_level)){
      _chunk_size *= GetNumberOfLeafNodeDegrees();
    }
    assert(_chunk_size >= number_of_blocks_per_cpu);
  }


  for(ui range(repeat_itr, 0, number_of_repeat)) {
    LOG_INFO("#%u) Evaluation", repeat_itr+1);
    //===--------------------------------------------------------------------===//
    // Prepare Hit & Node Visit Variables for an evaluation
    //===--------------------------------------------------------------------===//
    ui h_hit[GetNumberOfBlocks()];
    ui h_node_visit_count[GetNumberOfBlocks()];

    ui total_hit = 0;
    ui total_jump_count = 0;
    ui total_node_visit_count_cpu = 0;
    ui total_node_visit_count_gpu = 0;

    ui* d_hit;
    cudaErrCheck(cudaMalloc((void**) &d_hit, sizeof(ui)*GetNumberOfBlocks()));
    ui* d_node_visit_count;
    cudaErrCheck(cudaMalloc((void**) &d_node_visit_count, sizeof(ui)*GetNumberOfBlocks()));

    // initialize hit and node visit variables to zero
    global_SetHitCount2<<<1,GetNumberOfBlocks()>>>(0);
    cudaDeviceSynchronize();

    //===--------------------------------------------------------------------===//
    // Prepare Multi-thread Query Processing
    //===--------------------------------------------------------------------===//
    std::vector<std::thread> threads;
    ui thread_node_visit_count_cpu[number_of_cpu_threads];

    //===--------------------------------------------------------------------===//
    // Collect Start Node Index in Advance
    //===--------------------------------------------------------------------===//
    // NOTE : this code is for performance breakdown
    /*
#define USE_QUEUE
    // NOTE : Collect start node index in advance to measure GPU kernel launching time
    thread_start_node_index.resize(number_of_cpu_threads);
    // parallel for loop using c++ std 11 
    {
      auto chunk_size = number_of_search/number_of_cpu_threads;
      auto start_offset = 0 ;
      auto end_offset = start_offset + chunk_size + number_of_search%number_of_cpu_threads;

      for (ui range(thread_itr, 0, number_of_cpu_threads)) {
        threads.push_back(std::thread(&RTree_LS::Thread_CollectStartNodeIndex, this, 
              std::ref(query), std::ref(thread_start_node_index[thread_itr]),
              start_offset, end_offset));
        start_offset = end_offset;
        end_offset += chunk_size;
      }

      //Join the threads with the main thread
      for(auto &thread : threads){
        thread.join();
      }
    }
    threads.clear();
    */

    //===--------------------------------------------------------------------===//
    // Execute Search Function
    //===--------------------------------------------------------------------===//
    cudaProfilerStart();

    // launch the thread for monitoring as a background
    recorder.TimeRecordStart();

    // parallel for loop using c++ std 11 
    {
      auto search_chunk_size = number_of_search/number_of_cpu_threads;
      auto start_offset = 0 ;
      auto end_offset = start_offset + search_chunk_size + number_of_search%number_of_cpu_threads;

      for (ui range(thread_itr, 0, number_of_cpu_threads)) {
        threads.push_back(std::thread(&RTree_LS::Thread_Search, 
              this, std::ref(query), d_query, 
              thread_itr, number_of_blocks_per_cpu, 
              std::ref(thread_node_visit_count_cpu[thread_itr]),
              start_offset, end_offset));

        start_offset = end_offset;
        end_offset += search_chunk_size;
      }

      //Join the threads with the main thread
      for(auto &thread : threads){
        thread.join();
      }

      for(ui range(thread_itr, 0, number_of_cpu_threads)) {
        total_node_visit_count_cpu += thread_node_visit_count_cpu[thread_itr];
      }
    }

    // A problem with using host-device synchronization points, such as
    // cudaDeviceSynchronize(), is that they stall the GPU pipeline
    cudaDeviceSynchronize();

    global_GetHitCount2<<<1,GetNumberOfBlocks()>>>(d_hit, d_node_visit_count);
    cudaMemcpy(h_hit, d_hit, sizeof(ui)*GetNumberOfBlocks(), cudaMemcpyDeviceToHost);
    cudaMemcpy(h_node_visit_count, d_node_visit_count, sizeof(ui)*GetNumberOfBlocks(), 
               cudaMemcpyDeviceToHost);

    for(ui range(i, 0, GetNumberOfBlocks())) {
      total_hit += h_hit[i];
      total_node_visit_count_gpu += h_node_visit_count[i];
    }

    auto elapsed_time = recorder.TimeRecordEnd();

    // terminate the monitoring
    search_finish = true;

    cudaProfilerStop();

    LOG_INFO("Processing %uquery(ies) concurrently", number_of_cpu_threads);
    LOG_INFO("Avg. Search Time on the GPU (ms)\n%.6f", elapsed_time/(float)number_of_search);
    LOG_INFO("Total Search Time on the GPU (ms)%.6f", elapsed_time);

    //===--------------------------------------------------------------------===//
    // Show Results
    //===--------------------------------------------------------------------===//
    LOG_INFO("Hit : %u", total_hit);
    LOG_INFO("Avg. Node visit count on CPU : \n%f", total_node_visit_count_cpu/(float)number_of_search);
    LOG_INFO("Total Node visit count on CPU : %u", total_node_visit_count_cpu);
    LOG_INFO("Avg. Node visit count on GPU : \n%f", total_node_visit_count_gpu/(float)number_of_search);
    LOG_INFO("Total Node visit count on GPU : %d\n", total_node_visit_count_gpu);
  }
  return 1;
}

ll RTree_LS::GetNextStartNodeIndex(ui tid) {
  auto start_node_index =  thread_start_node_index[tid].front();
  thread_start_node_index[tid].pop();
  return start_node_index;
}

void RTree_LS::Thread_Search(std::vector<Point>& query, Point* d_query, ui tid,
                           ui number_of_blocks_per_cpu, ui& node_visit_count, 
                           ui start_offset, ui end_offset) {
  node_visit_count = 0;
  ui query_offset = start_offset*GetNumberOfDims()*2;
  const ui bid_offset = tid*number_of_blocks_per_cpu;

  for(ui range(query_itr, start_offset, end_offset)) {
      RTree_LS_Search(); //node_ptr, &query[query_offset], d_query); 
//                      query_offset, bid_offset, number_of_blocks_per_cpu, 
//                      &node_visit_count);
      query_offset += GetNumberOfDims()*2;
  }
}

ui RTree_LS::GetChunkSize() const{
  return chunk_size;
}

void RTree_LS::SetUpperTreeType(TreeType _UPPER_TREE_TYPE){
  UPPER_TREE_TYPE = _UPPER_TREE_TYPE;
  assert(UPPER_TREE_TYPE);
}

void RTree_LS::SetChunkSize(ui _chunk_size){
  ui* p_chunk_size = (ui*)&chunk_size;
  *p_chunk_size = _chunk_size;
  assert(chunk_size);
}

void RTree_LS::SetChunkUpdated(bool updated){
  {
    std::lock_guard<std::mutex> lock(chunk_updated_mutex);
    chunk_updated = updated;
  }
}

void RTree_LS::SetScanLevel(ui _scan_level){
  ui* p_scan_level = (ui*)&scan_level;
  *p_scan_level = _scan_level;
  assert(scan_level);
}

void RTree_LS::SetNumberOfCPUThreads(ui _number_of_cpu_threads){
  ui* p_number_of_cpu_threads = (ui*)&number_of_cpu_threads;
  *p_number_of_cpu_threads = _number_of_cpu_threads;
  assert(number_of_cuda_blocks/number_of_cpu_threads>0);
}

void RTree_LS::SetNumberOfCUDABlocks(ui _number_of_cuda_blocks){
  number_of_cuda_blocks = _number_of_cuda_blocks;
  assert(number_of_cuda_blocks);
}

void RTree_LS::RTree_LS_Search(){ //node::Node *node_ptr, Point* query, Point* d_query){
/*
                               ui query_offset, ui bid_offset, ui number_of_blocks_per_cpu, 
                               ui *node_visit_count) {
  (*node_visit_count)++;

  // internal nodes
  if(node_ptr->GetNodeType() == NODE_TYPE_INTERNAL ) {
    for(ui range(branch_itr, 0, node_ptr->GetBranchCount())) {
      if( node_ptr->IsOverlap(query, branch_itr)) {
          RTree_LS_Search(node_ptr->GetBranchChildNode(branch_itr), query, d_query,
                          query_offset, bid_offset, number_of_blocks_per_cpu, node_visit_count);
      }
    }
  } // leaf nodes
  else {
      auto start_node_offset = node_ptr->GetBranchChildOffset(0);
      global_RTree_LeafNode_Scan<<<number_of_blocks_per_cpu,GetNumberOfThreads()>>>
                                  (&d_query[query_offset], start_node_offset, 
                                   chunk_size,//this should be leaf node size
                                   bid_offset, number_of_blocks_per_cpu );

      // wait until it's finished
      cudaDeviceSynchronize();
  }
  */
}

//===--------------------------------------------------------------------===//
// Cuda Variable & Function 
//===--------------------------------------------------------------------===//

__device__ ui g_hit2[GetNumberOfMAXBlocks()]; 
__device__ ui g_node_visit_count2[GetNumberOfMAXBlocks()]; 

__device__ ui g_monitor2[GetNumberOfMAXBlocks()]; 

__global__
void global_SetHitCount2(ui init_value) {
  int tid = threadIdx.x;

  g_hit2[tid] = init_value;
  g_node_visit_count2[tid] = init_value;
  g_monitor2[tid] = init_value;
}

__global__
void global_GetHitCount2(ui* hit, ui* node_visit_count) {
  int tid = threadIdx.x;

  hit[tid] = g_hit2[tid];
  node_visit_count[tid] = g_node_visit_count2[tid];
}

__global__
void global_GetMonitor2(ui* monitor) {
  int tid = threadIdx.x;

  monitor[tid] = g_monitor2[tid];
}

//===--------------------------------------------------------------------===//
// Scan Leaf Nodes
//===--------------------------------------------------------------------===//
__global__ 
void global_RTree_LeafNode_Scan(Point* _query, ll start_node_offset, 
                               ui chunk_size, ui bid_offset, 
                               ui number_of_blocks_per_cpu) {
  int bid = blockIdx.x;
  int tid = threadIdx.x;

  __shared__ ui t_hit[GetNumberOfThreads()]; 
  __shared__ Point query[GetNumberOfDims()*2];

  if(tid < GetNumberOfDims()*2) {
    query[tid] = _query[tid];
  }

  t_hit[tid] = 0;
  g_monitor2[bid+bid_offset]=0;

  node::Node_SOA* node_soa_ptr = manager::g_node_soa_ptr/*first leaf node*/ + start_node_offset + bid;
  __syncthreads();

  //===--------------------------------------------------------------------===//
  // Leaf Nodes
  //===--------------------------------------------------------------------===//

  for(ui range(node_itr, bid, chunk_size, number_of_blocks_per_cpu)) {

    MasterThreadOnly {
      g_node_visit_count2[bid+bid_offset]++;
    }

    if(tid < node_soa_ptr->GetBranchCount()) {
        if(node_soa_ptr->IsOverlap(query, tid)) {
          t_hit[tid]++;
          MasterThreadOnly {
            g_monitor2[bid+bid_offset]=0;
          }
        } else {
          MasterThreadOnly {
            g_monitor2[bid+bid_offset]++;
          }
        }
    }
    __syncthreads();

    node_soa_ptr+=number_of_blocks_per_cpu;
  }
  __syncthreads();

  //===--------------------------------------------------------------------===//
  // Parallel Reduction 
  //===--------------------------------------------------------------------===//
  ParallelReduction(t_hit, GetNumberOfThreads());

  MasterThreadOnly {
    g_hit2[bid+bid_offset] += t_hit[0] + t_hit[1];
  }
}

} // End of tree namespace
} // End of ursus namespace

