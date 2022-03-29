/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*!
 * \file graph_executor.cc
 */
#include "graph_executor.h"

#include <tvm/runtime/container/map.h>
#include <tvm/runtime/container/string.h>
#include <tvm/runtime/data_type.h>
#include <tvm/runtime/device_api.h>
#include <tvm/runtime/ndarray.h>
#include <tvm/runtime/packed_func.h>
#include <tvm/runtime/profiling.h>
#include <tvm/runtime/registry.h>
#include <tvm/runtime/serializer.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <numeric>
#include <ostream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>
#include <random>

#include "../file_utils.h"

namespace tvm {
namespace runtime {
namespace details {
inline size_t GetDataAlignment(const DLTensor& arr) {
  size_t align = (arr.dtype.bits / 8) * arr.dtype.lanes;
  if (align < kAllocAlignment) return kAllocAlignment;
  return align;
}
}  // namespace details

/*!
 * \brief Run all the operations one by one.
 */
void GraphExecutor::Run() {
  ref_cnt.resize(num_node_entries(), 0);
  // setup the array and requirements.
  for (size_t i = 0; i < op_execs_.size(); ++i) {
    counter += 1;
    if (op_execs_[i]) PerformOp(i);
  }
}

void GraphExecutor::RematerializeTensor(size_t nid) { PerformOp(nid); }

void GraphExecutor::PerformOp(size_t nid) {
  auto node = nodes_[nid];
  // first we need to get all the input tensors and add the reference count
  // if they are not in memory, rematerialize them
  for (const auto& input : node.inputs) {
    // we do not need to care about input and parameter
    if (std::find(input_nodes_.begin(), input_nodes_.end(), entry_id(input)) != input_nodes_.end())
      continue;
    // otherwise we should rematerialize the input tensors that are not in the memory
    // and update the reference count at the same time
    ref_cnt[entry_id(input)] += 1;
    auto id = entry_id(input);
    if (is_evicted(entry_id(input)))
      RematerializeTensor(entry_id(input));
    ICHECK_GE(ref_cnt[entry_id(input)],1);
  }
  // now all of the inputs are recovered, we can set up to compute the location of input/output
  // compute current input memory location of the tensor
  std::unordered_set<uint32_t> input_node_eids;
  for (unsigned int nid_ : input_nodes_) {
    input_node_eids.insert(entry_id(nid_, 0));
  }
  std::unordered_set<uint32_t> output_node_eids;
  for (auto& output : outputs_) {
    output_node_eids.insert(entry_id(output));
  }

  const auto& inode = nodes_[nid];
  std::vector<DLTensor> args;

  for (const auto& e : inode.inputs) {
    uint32_t eid = this->entry_id(e);
    args.push_back(*(data_entry_[eid].operator->()));
  }

  /*
   * Note that some output tensors might not be allocated because some op might produce plural
   * values Thus we should check if the outputs were allocated and set up the data entry as well
   */
  for (uint32_t index = 0; index < inode.param.num_outputs; ++index) {
    uint32_t eid = this->entry_id(nid, index);
    set_tensor(eid);
    // ref_cnt[eid] += 1;
    args.push_back(*(data_entry_[eid].operator->()));
  }

  // Now we should update the memory location in TVM Operator
  std::shared_ptr<OpArgs> updated_args = UpdateTVMOp(nid, nodes_[nid].param, args);

  UpdateInputOutputTensors(input_node_eids, output_node_eids, nid, updated_args);

  // now we can execute the operator
  op_execs_[nid]();

  // After this we need to decrease the reference count of all the input tensors
  // since we have finished using them

  for(const auto& input: node.inputs){
    // Again, continues when meet input and parameters
    if (std::find(input_nodes_.begin(), input_nodes_.end(), entry_id(input)) != input_nodes_.end())
      continue;
    // decrease the reference count of input tensor
    ref_cnt[entry_id(input)] -= 1;
  }
  // at last, we should perform eviction
  PerformEviction(nid);
}

void GraphExecutor::PerformEviction(size_t curr_tensor) {
  for(size_t i = 0; i < curr_tensor;i++){
    // do not do anything on input and parameters
    if (std::find(input_nodes_.begin(), input_nodes_.end(), i) != input_nodes_.end()) continue;
    // And we can perform eviction on the input tensors whose ref_cnt is 0.
    if (!ref_cnt[i]){
      GraphExecutor::EvictTensor(i,curr_tensor);
    }
  }
}

void GraphExecutor::FreeTensor(size_t nid,size_t curr_tensor) {
  // ICHECK_NE(get_sid(nid), get_sid(curr_tensor));
  // first we need to clean all the data entries
  bool flag = true;
  for (size_t i = 0; i < curr_tensor; i++) {
    if (ref_cnt[i]) {
      flag = false; continue;
    }
    if (std::find(input_nodes_.begin(), input_nodes_.end(), i) == input_nodes_.end() &&
        get_sid(nid) == get_sid(i) && data_entry_[i].defined())
      data_entry_[i].reset();
  }
  // then we clean the storage pool
  if (get_sid(nid) != get_sid(curr_tensor) && flag)
    storage_pool_[get_sid(nid)].reset();
}

void GraphExecutor::EvictTensor(size_t nid,size_t curr_tensor) {
  if (Heuristic(nid)) FreeTensor(nid,curr_tensor);
}
bool GraphExecutor::Heuristic(size_t nid) {
  std::default_random_engine rng_gen;
  std::normal_distribution<double> distribution(0,1);
  auto num = static_cast<size_t>(fmul(distribution(rng_gen), 100.0));
  LOG_INFO << num;
  return num % 2;
}
/*!
 * \brief Initialize the graph executor with graph and device.
 * \param graph_json The execution graph.
 * \param module The module containing the compiled functions for the host
 * processor.
 * \param devs The devices of the host and devices where graph nodes will be
 * executed on.
 * \param lookup_linked_param_func Linked parameter lookup function. Default is nullptr.
 */
void GraphExecutor::Init(const std::string& graph_json, tvm::runtime::Module module,
                         const std::vector<Device>& devs,
                         const PackedFunc lookup_linked_param_func) {
  counter = 0;
  std::istringstream is(graph_json);
  dmlc::JSONReader reader(&is);
  this->Load(&reader);
  module_ = module;
  devices_ = devs;
  lookup_linked_param_ = lookup_linked_param_func;
  if (lookup_linked_param_ == nullptr) {
    lookup_linked_param_ = PackedFunc(
        [this](TVMArgs args, TVMRetValue* rv) { this->DefaultLookupLinkedParam(args, rv); });
  }
  this->SetupStorage();
  this->SetupOpExecs();
  for (size_t i = 0; i < input_nodes_.size(); i++) {
    const uint32_t nid = input_nodes_[i];
    std::string& name = nodes_[nid].name;
    input_map_[name] = i;
  }
  for (size_t i = 0; i < outputs_.size(); i++) {
    const uint32_t nid = outputs_[i].node_id;
    std::string& name = nodes_[nid].name;
    output_map_[name] = i;
  }
}

/*!
 * \brief Get the input index given the name of input.
 * \param name The name of the input.
 * \return The index of input.
 */
int GraphExecutor::GetInputIndex(const std::string& name) {
  auto it = input_map_.find(name);
  if (it != input_map_.end()) {
    return it->second;
  }
  return -1;
}

/*!
 * \brief Get the input info of Graph by parsing the input nodes.
 * \return The shape and dtype tuple.
 */
std::tuple<GraphExecutor::ShapeInfo, GraphExecutor::DtypeInfo> GraphExecutor::GetInputInfo() const {
  GraphExecutor::ShapeInfo shape_dict;
  GraphExecutor::DtypeInfo dtype_dict;
  for (uint32_t nid : input_nodes_) {
    CHECK_LE(nid, nodes_.size());
    std::string name = nodes_[nid].name;
    if (param_names_.find(name) == param_names_.end()) {
      CHECK_LE(nid, attrs_.shape.size());
      auto shape = attrs_.shape[nid];
      shape_dict.Set(name, ShapeTuple(shape));
      CHECK_LE(nid, attrs_.dltype.size());
      auto dtype = attrs_.dltype[nid];
      dtype_dict.Set(name, String(dtype));
    }
  }
  return std::make_tuple(shape_dict, dtype_dict);
}

/*!
 * \brief Get the output index given the name of output.
 * \param name The name of the output.
 * \return The index of output.
 */
int GraphExecutor::GetOutputIndex(const std::string& name) {
  auto it = output_map_.find(name);
  if (it != output_map_.end()) {
    return it->second;
  }
  return -1;
}
/*!
 * \brief set index-th input to the graph.
 * \param index The input index.
 * \param data_in The input data.
 */
void GraphExecutor::SetInput(int index, DLTensor* data_in) {
  ICHECK_LT(static_cast<size_t>(index), input_nodes_.size());
  uint32_t eid = this->entry_id(input_nodes_[index], 0);
  data_entry_[eid].CopyFrom(data_in);
}
/*!
 * \brief Check the legality of external DLTensor*.
 * \param external The external DLTensor*.
 * \param eid The data_enrty_ index.
 */
void GraphExecutor::CheckExternalDLTensor(const DLTensor* external, uint32_t eid) const {
  const DLTensor* internal = data_entry_[eid].operator->();

  ICHECK_EQ(data_alignment_[eid], details::GetDataAlignment(*external));
  ICHECK_EQ(reinterpret_cast<size_t>(external->data) % kAllocAlignment, 0);
  ICHECK_EQ(internal->ndim, static_cast<size_t>(external->ndim));
  ICHECK_EQ(internal->device.device_type, external->device.device_type);
  ICHECK_EQ(internal->device.device_id, external->device.device_id);
  for (auto i = 0; i < external->ndim; ++i) {
    ICHECK_EQ(internal->shape[i], external->shape[i]);
  }
}
/*!
 * \brief set index-th input to the graph without copying the data.
 * \param index The input index.
 * \param data_ref The input data that is referred.
 */
void GraphExecutor::SetInputZeroCopy(int index, DLTensor* data_ref) {
  ICHECK_LT(static_cast<size_t>(index), input_nodes_.size());
  uint32_t eid = this->entry_id(input_nodes_[index], 0);
  // check the consistency of input
  CheckExternalDLTensor(data_ref, eid);
  // Update the data pointer for each argument of each op
  for (DLTensor* t : input_dltensors_[eid]) {
    t->data = data_ref->data;
  }
}
/*!
 * \brief set index-th output to the graph without copying the data.
 * \param index The output index.
 * \param data_ref The output data that is referred.
 */
void GraphExecutor::SetOutputZeroCopy(int index, DLTensor* data_ref) {
  ICHECK_LT(static_cast<size_t>(index), outputs_.size());
  ICHECK_LT(static_cast<size_t>(index), output_dltensors_.size());
  const NodeEntry& output_node = outputs_[index];
  uint32_t output_node_eid = this->entry_id(output_node);

  // check the consistency of output
  CheckExternalDLTensor(data_ref, output_node_eid);

  // Update the data pointer for output op
  for (DLTensor* t : output_dltensors_[output_node_eid]) {
    t->data = data_ref->data;
  }

  // Update the input of the op connected to the output
  for (DLTensor* t : both_output_opinput_dltensors_[output_node_eid]) {
    t->data = data_ref->data;
  }
}
/*!
 * \brief Get the number of outputs
 *
 * \return The number of outputs from graph.
 */
int GraphExecutor::NumOutputs() const { return outputs_.size(); }
/*!
 * \brief Get the number of inputs
 *
 * \return The number of inputs to the graph.
 */
int GraphExecutor::NumInputs() const { return input_nodes_.size(); }
/*!
 * \brief Return NDArray for given input index.
 * \param index The input index.
 *
 * \return NDArray corresponding to given input node index.
 */
NDArray GraphExecutor::GetInput(int index) const {
  ICHECK_LT(static_cast<size_t>(index), input_nodes_.size());
  uint32_t eid = this->entry_id(input_nodes_[index], 0);
  return data_entry_[eid];
}
/*!
 * \brief Return NDArray for given output index.
 * \param index The output index.
 *
 * \return NDArray corresponding to given output node index.
 */
NDArray GraphExecutor::GetOutput(int index) const {
  ICHECK_LT(static_cast<size_t>(index), outputs_.size());
  uint32_t eid = this->entry_id(outputs_[index]);
  return data_entry_[eid];
}
/*!
 * \brief Copy index-th output to data_out.
 * \param index The output index.
 * \param data_out the output data.
 */
void GraphExecutor::CopyOutputTo(int index, DLTensor* data_out) {
  ICHECK_LT(static_cast<size_t>(index), outputs_.size());
  uint32_t eid = this->entry_id(outputs_[index]);

  // Check the shapes to avoid receiving in different dimension but same size.
  const NDArray& data = data_entry_[eid];
  ICHECK_EQ(data->ndim, data_out->ndim);
  for (int32_t j = 0; j < data->ndim; ++j) {
    ICHECK_EQ(data->shape[j], data_out->shape[j]);
  }

  data_entry_[eid].CopyTo(data_out);
}

/*!
 * \brief Load parameters from parameter blob.
 * \param param_blob A binary blob of parameter.
 */
void GraphExecutor::LoadParams(const std::string& param_blob) {
  dmlc::MemoryStringStream strm(const_cast<std::string*>(&param_blob));
  this->LoadParams(&strm);
}

void GraphExecutor::LoadParams(dmlc::Stream* strm) {
  Map<String, NDArray> params = ::tvm::runtime::LoadParams(strm);
  for (auto& p : params) {
    param_names_.insert(p.first);
    int in_idx = GetInputIndex(p.first);
    if (in_idx < 0) continue;
    uint32_t eid = this->entry_id(input_nodes_[in_idx], 0);
    data_entry_[eid].CopyFrom(p.second);
  }
}

void GraphExecutor::ShareParams(const GraphExecutor& other, dmlc::Stream* strm) {
  uint64_t header, reserved;
  ICHECK(strm->Read(&header)) << "Invalid parameters file format";
  ICHECK(header == kTVMNDArrayListMagic) << "Invalid parameters file format";
  ICHECK(strm->Read(&reserved)) << "Invalid parameters file format";
  std::vector<std::string> names;
  ICHECK(strm->Read(&names)) << "Invalid parameters file format";
  uint64_t sz;
  strm->Read(&sz);
  size_t size = static_cast<size_t>(sz);
  ICHECK(size == names.size()) << "Invalid parameters file format";
  for (size_t i = 0; i < size; ++i) {
    int in_idx = GetInputIndex(names[i]);
    if (in_idx < 0) continue;
    uint32_t eid = this->entry_id(input_nodes_[in_idx], 0);
    ICHECK_LT(eid, data_entry_.size());
    ICHECK_EQ(data_entry_[eid].use_count(), 1);
    data_entry_[eid] = other.GetInput(GetInputIndex(names[i]));
    ICHECK_GT(data_entry_[eid].use_count(), 1);
    const DLTensor* tmp = data_entry_[eid].operator->();
    data_alignment_[eid] = details::GetDataAlignment(*tmp);
  }
  this->SetupOpExecs();
}

void GraphExecutor::LinkedNDArrayDeleter(Object* container) {
  // container is the NDArray::Container which needs to get deleted.
  // The data member points to global const memory, so it does not need deleting.
  delete static_cast<NDArray::Container*>(container);
}

void GraphExecutor::DefaultLookupLinkedParam(TVMArgs args, TVMRetValue* rv) {
  Module mod = args[0];
  int64_t storage_id = args[1];
  DLTensor* template_tensor = args[2];
  Device dev = args[3];
  // Get pre-linked parameter lookup function, if it was generated. When pf == nullptr, no linked
  // params are present.
  if (!module_lookup_linked_param_valid_) {
    module_lookup_linked_param_ =
        mod.GetFunction(::tvm::runtime::symbol::tvm_lookup_linked_param, true);
  }
  if (module_lookup_linked_param_ == nullptr) {
    *rv = nullptr;
    return;
  }

  TVMRetValue opaque_handle = module_lookup_linked_param_(storage_id);
  if (opaque_handle.type_code() == kTVMNullptr) {
    *rv = nullptr;
    return;
  }

  std::vector<int64_t> shape_vec{template_tensor->shape,
                                 template_tensor->shape + template_tensor->ndim};

  auto* container = new NDArray::Container(static_cast<void*>(opaque_handle), shape_vec,
                                           template_tensor->dtype, dev);
  container->SetDeleter(GraphExecutor::LinkedNDArrayDeleter);
  *rv = NDArray(GetObjectPtr<Object>(container));
}

void GraphExecutor::AllocTensor(size_t sid) {
  const auto& pit = pool_entry_[sid];
  // This for loop is very fast since there are usually only a couple of
  // devices available on the same hardware.
  const auto& cit = std::find_if(devices_.begin(), devices_.end(), [&pit](const Device& d) {
    return pit.device_type == static_cast<int>(d.device_type);
  });
  Device dev = cit == devices_.end() ? devices_[0] : *cit;
  if (pit.linked_param.defined()) {
    storage_pool_[sid] = pit.linked_param;
  } else {
    std::vector<int64_t> shape;
    shape.push_back(static_cast<int64_t>(pit.size + 3) / 4);
    storage_pool_[sid] = NDArray::Empty(shape, DLDataType{kDLFloat, 32, 1}, dev);
  }
}

void GraphExecutor::SetupStorage() {
  // Grab saved optimization plan from graph.
  std::vector<DLDataType> vtype;
  for (const std::string& s_type : attrs_.dltype) {
    vtype.push_back(tvm::runtime::String2DLDataType(s_type));
  }

  // Size and device type of each storage pool entry.
  std::vector<PoolEntry> pool_entry;
  // Find the maximum space size.
  for (size_t i = 0; i < attrs_.shape.size(); ++i) {
    int storage_id = attrs_.storage_id[i];
    // Use the fallback device if no device index is available.
    int device_type = static_cast<int>(devices_[0].device_type);
    if (!attrs_.device_index.empty()) {
      device_type = attrs_.device_index[i];
    }
    size_t size = 1;
    for (int64_t sz : attrs_.shape[i]) {
      size *= static_cast<size_t>(sz);
    }
    ICHECK_GE(storage_id, 0) << "Do not support runtime shape op";
    DLDataType t = vtype[i];
    size_t bits = t.bits * t.lanes;
    ICHECK(bits % 8U == 0U || bits == 1U || bits == 4U);
    size_t bytes = ((bits + 7U) / 8U) * size;

    uint32_t sid = static_cast<uint32_t>(storage_id);
    if (sid >= pool_entry.size()) {
      pool_entry.resize(sid + 1, {0, -1});
    } else {
      ICHECK(pool_entry[sid].device_type == -1 || pool_entry[sid].device_type == device_type)
          << "The same pool entry cannot be assigned to multiple devices";
    }
    TVMRetValue lookup_rv;
    {
      std::vector<int64_t> shape_vec{attrs_.shape[i].begin(), attrs_.shape[i].end()};
      DLTensor template_tensor{nullptr,  Device{kDLCPU, 0}, static_cast<int>(shape_vec.size()),
                               vtype[i], shape_vec.data(),  nullptr,
                               0};
      lookup_rv = lookup_linked_param_(module_, sid, &template_tensor, devices_[0]);
    }
    if (lookup_rv.type_code() != kTVMNullptr) {
      pool_entry[sid].linked_param = lookup_rv;
    }
    pool_entry[sid].param_data_entry = i;
    pool_entry[sid].size = std::max(pool_entry[sid].size, bytes);
    pool_entry[sid].device_type = device_type;
  }
  pool_entry_ = std::move(pool_entry);
  // First we compute the map of sid to input nodes
  std::unordered_map<int, uint32_t> sid_to_input;
  for (unsigned int input_node : input_nodes_) {
    auto input_sid = get_sid(input_node);
    sid_to_input[input_sid] = input_node;
  }
  // Allocate the space for argument tensor
  storage_pool_.resize(pool_entry_.size());
  for (size_t sid = 0; sid < pool_entry_.size(); sid++) {
    // We just allocate tensor for input nodes now
    if (sid_to_input.find(static_cast<int>(sid)) != sid_to_input.end()) {
      AllocTensor(sid);
    }
  }

  // Assign the pooled entries. A unified memory pool is used to simplifiy
  // memory assignment for each node entry. The allocated memory on each device
  // is mapped to this pool.
  data_entry_.resize(num_node_entries());
  data_alignment_.resize(num_node_entries());
  for (size_t i = 0; i < data_entry_.size(); ++i) {
    int storage_id = attrs_.storage_id[i];
    ICHECK_LT(static_cast<size_t>(storage_id), storage_pool_.size());
    // just make view for allocated parameters
    if (std::find(input_nodes_.begin(), input_nodes_.end(), i) != input_nodes_.end()) {
      data_entry_[i] = storage_pool_[storage_id].CreateView(attrs_.shape[i], vtype[i]);
    }

    // determine memory usage using grabbed vtype
    size_t align = (vtype[i].bits / 8) * vtype[i].lanes;
    data_alignment_[i] = align < kAllocAlignment ? kAllocAlignment : align;
  }
}

void GraphExecutor::SetupOpExecs() {
  op_execs_.resize(this->GetNumOfNodes());
  input_dltensors_.resize(num_node_entries());
  output_dltensors_.resize(num_node_entries());
  both_output_opinput_dltensors_.resize(num_node_entries());
  std::unordered_set<uint32_t> input_node_eids;
  for (size_t i = 0; i < input_nodes_.size(); i++) {
    uint32_t nid = input_nodes_[i];
    input_node_eids.insert(entry_id(nid, 0));
  }
  std::unordered_set<uint32_t> output_node_eids;
  for (size_t i = 0; i < outputs_.size(); i++) {
    output_node_eids.insert(entry_id(outputs_[i]));
  }

  // setup the array and requirements.
  for (uint32_t nid = 0; nid < this->GetNumOfNodes(); ++nid) {
    const auto& inode = nodes_[nid];
    if (inode.op_type == "null") continue;
    std::vector<DLTensor> args = {};

    std::shared_ptr<OpArgs> op_args = nullptr;
    std::tie(op_execs_[nid], op_args) = CreateTVMOp(nid, inode.param, args);
  }
}

void GraphExecutor::UpdateInputOutputTensors(const std::unordered_set<uint32_t>& input_node_eids,
                                             const std::unordered_set<uint32_t>& output_node_eids,
                                             uint32_t nid,
                                             std::shared_ptr<GraphExecutor::OpArgs> op_args) {
  const auto& inode = nodes_[nid];
  for (size_t i = 0; i < inode.inputs.size(); i++) {
    uint32_t input_eid = this->entry_id(inode.inputs[i]);
    // check if op input is model input
    if (input_node_eids.count(input_eid) > 0) {
      auto& eid_input = input_dltensors_[input_eid];
      auto input_tensor = static_cast<DLTensor*>(op_args->arg_values[i].v_handle);
      if (std::find(eid_input.begin(), eid_input.end(), input_tensor) == eid_input.end()) {
        eid_input.push_back(input_tensor);
      }
    }
    // check if any model output is the input of the op
    if (output_node_eids.count(input_eid) > 0) {
      auto& eid_both = both_output_opinput_dltensors_[input_eid];
      auto both_tensor = static_cast<DLTensor*>(op_args->arg_values[i].v_handle);
      if (std::find(eid_both.begin(), eid_both.end(), both_tensor) == eid_both.end()) {
        eid_both.push_back(both_tensor);
      }
    }
  }

  for (uint32_t i = inode.inputs.size(); i < inode.inputs.size() + inode.param.num_outputs; ++i) {
    uint32_t output_eid = this->entry_id(nid, i - inode.inputs.size());
    // check if op output is model output
    if (output_node_eids.count(output_eid) > 0) {
      auto& eid_out = output_dltensors_[output_eid];
      auto out_tensor = static_cast<DLTensor*>(op_args->arg_values[i].v_handle);
      if (std::find(eid_out.begin(), eid_out.end(), out_tensor) == eid_out.end()) {
        eid_out.push_back(out_tensor);
      }
    }
  }
}
std::pair<std::function<void()>, std::shared_ptr<GraphExecutor::OpArgs> >
GraphExecutor::CreateTVMOp(uint32_t nid, const TVMOpParam& param,
                           const std::vector<DLTensor>& args) {
  std::shared_ptr<GraphExecutor::OpArgs> arg_ptr = std::make_shared<GraphExecutor::OpArgs>();
  // set up the memory location map
  op_mem_location_map_[nid] = arg_ptr;

  if (param.func_name == "__nop") {
    return {[]() {}, arg_ptr};
  } else if (param.func_name == "__copy") {
    // Perform cross device data copy.
    // Directly copy data from the input to the output.
    // TODO(mbs): device_copy cleanup.
    auto fexec = [nid, this]() {
      auto arg_ptr = this->op_mem_location_map_[nid];
      DLTensor* from = static_cast<DLTensor*>(arg_ptr->arg_values[0].v_handle);
      DLTensor* to = static_cast<DLTensor*>(arg_ptr->arg_values[1].v_handle);
      TVM_CCALL(TVMArrayCopyFromTo(from, to, nullptr));
    };
    return {fexec, arg_ptr};
  }

  // Get compiled function from the module that contains both host and device
  // code.
  tvm::runtime::PackedFunc pf = module_.GetFunction(param.func_name, true);
  ICHECK(pf != nullptr) << "no such function in module: " << param.func_name;

  auto fexec = [pf, nid, this]() {
    TVMRetValue rv;
    auto arg_ptr = this->op_mem_location_map_[nid];
    TVMArgs targs(arg_ptr->arg_values.data(), arg_ptr->arg_tcodes.data(),
                  static_cast<int>(arg_ptr->arg_values.size()));
    pf.CallPacked(targs, &rv);
  };
  return {fexec, arg_ptr};
}

PackedFunc GraphExecutor::GetFunction(const std::string& name,
                                      const ObjectPtr<Object>& sptr_to_self) {
  // Return member functions during query.
  if (name == "set_input") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0])) {
        int in_idx = this->GetInputIndex(args[0].operator String());
        if (in_idx >= 0) this->SetInput(in_idx, args[1]);
      } else {
        this->SetInput(args[0], args[1]);
      }
    });
  } else if (name == "set_input_zero_copy") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0])) {
        int in_idx = this->GetInputIndex(args[0].operator String());
        if (in_idx >= 0) this->SetInputZeroCopy(in_idx, args[1]);
      } else {
        this->SetInputZeroCopy(args[0], args[1]);
      }
    });
  } else if (name == "set_output_zero_copy") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (String::CanConvertFrom(args[0])) {
        int out_idx = this->GetOutputIndex(args[0].operator String());
        if (out_idx >= 0) this->SetOutputZeroCopy(out_idx, args[1]);
      } else {
        this->SetOutputZeroCopy(args[0], args[1]);
      }
    });
  } else if (name == "get_output") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      if (args.num_args == 2) {
        this->CopyOutputTo(args[0], args[1]);
      } else {
        *rv = this->GetOutput(args[0]);
      }
    });
  } else if (name == "get_input") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      int in_idx = 0;
      if (String::CanConvertFrom(args[0])) {
        in_idx = this->GetInputIndex(args[0].operator String());
      } else {
        in_idx = args[0];
      }
      if (in_idx >= 0) {
        *rv = this->GetInput(in_idx);
      }
    });
  } else if (name == "get_num_outputs") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->NumOutputs(); });
  } else if (name == "get_num_inputs") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { *rv = this->NumInputs(); });
  } else if (name == "run") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) { this->Run(); });
  } else if (name == "run_from_inputs") {
    return PackedFunc(
        [sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
          CHECK(args.size() % 2 == 0)
              << "Number of arguments to run_from_inputs must be an even number of key-value pairs";
          Device host{static_cast<DLDeviceType>(args[0].operator int()), args[1].operator int()};
          for (int i = 2; i < args.size(); i += 2) {
            if (String::CanConvertFrom(args[i])) {
              int in_idx = this->GetInputIndex(args[i].operator String());
              if (in_idx >= 0) {
                this->SetInput(in_idx, args[i + 1]);
              } else {
                LOG(FATAL) << args[i].operator String() << " is not a valid input name";
              }
            } else {
              this->SetInput(args[i], args[i + 1]);
            }
          }
          this->Run();
          Array<NDArray> outputs;
          for (int i = 0; i < this->NumOutputs(); i++) {
            NDArray out = this->GetOutput(i);
            NDArray a = NDArray::Empty(out.Shape(), out.DataType(), host);
            a.CopyFrom(out);
            outputs.push_back(a);
          }
          *rv = outputs;
        });
  } else if (name == "load_params") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      this->LoadParams(args[0].operator std::string());
    });
  } else if (name == "share_params") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      const auto& module = args[0].operator Module();
      ICHECK_EQ(module.operator->()->type_key(), std::string("GraphExecutor"));
      const auto& param_blob = args[1].operator std::string();
      dmlc::MemoryStringStream strm(const_cast<std::string*>(&param_blob));
      this->ShareParams(dynamic_cast<const GraphExecutor&>(*module.operator->()), &strm);
    });
  } else if (name == "get_input_index") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      CHECK(String::CanConvertFrom(args[0])) << "Input key is not a string";
      *rv = this->GetInputIndex(args[0].operator String());
    });
  } else if (name == "get_input_info") {
    return PackedFunc([sptr_to_self, this](TVMArgs args, TVMRetValue* rv) {
      GraphExecutor::ShapeInfo shape_info;
      GraphExecutor::DtypeInfo dtype_info;
      std::tie(shape_info, dtype_info) = this->GetInputInfo();
      Map<String, ObjectRef> input_info;
      input_info.Set("shape", shape_info);
      input_info.Set("dtype", dtype_info);
      *rv = input_info;
    });
  } else {
    return PackedFunc();
  }
}
std::shared_ptr<GraphExecutor::OpArgs> GraphExecutor::UpdateTVMOp(
    uint32_t nid, const TVMOpParam& param, const std::vector<DLTensor>& args) {
  auto arg_ptr = op_mem_location_map_[nid];
  arg_ptr->arg_values.clear();
  arg_ptr->arg_tcodes.clear();
  // setup address.
  arg_ptr->args = args;
  if (param.flatten_data) {
    arg_ptr->shape_data.resize(arg_ptr->args.size());
  }
  for (size_t i = 0; i < arg_ptr->args.size(); ++i) {
    TVMValue v;
    DLTensor* t = &arg_ptr->args[i];
    v.v_handle = t;
    arg_ptr->arg_values.push_back(v);
    arg_ptr->arg_tcodes.push_back(kTVMDLTensorHandle);
    if (param.flatten_data) {
      arg_ptr->shape_data[i] =
          std::accumulate(t->shape, t->shape + t->ndim, 1, std::multiplies<int64_t>());
      t->ndim = 1;
      t->shape = &(arg_ptr->shape_data[i]);
    }
  }
  return arg_ptr;
}


Module GraphExecutorCreate(const std::string& sym_json, const tvm::runtime::Module& m,
                           const std::vector<Device>& devs,
                           const PackedFunc lookup_linked_param_func) {
  auto exec = make_object<GraphExecutor>();
  exec->Init(sym_json, m, devs, lookup_linked_param_func);
  return Module(exec);
}

// Get all devices for the host and other runtime devices.
std::vector<Device> GetAllDevice(const TVMArgs& args, int dev_start_arg) {
  // Reserve the first item as the fallback device.
  std::vector<Device> ret;
  Device dev;
  for (int i = dev_start_arg; i < args.num_args; i += 2) {
    int dev_type = args[i];
    dev.device_type = static_cast<DLDeviceType>(dev_type);
    dev.device_id = args[i + 1];
    ret.push_back(dev);
  }
  return ret;
}

// 4-argument version is currently reserved to keep support of calling
// from tvm4j and javascript, since they don't have heterogeneous
// execution support yet. For heterogenenous execution, at least 5 arguments will
// be passed in. The third one is the number of devices.
// Eventually, we will only probably pass Device for all the languages.
TVM_REGISTER_GLOBAL("tvm.graph_executor.create").set_body([](TVMArgs args, TVMRetValue* rv) {
  ICHECK_GE(args.num_args, 4) << "The expected number of arguments for graph_executor.create is "
                                 "at least 4, but it has "
                              << args.num_args;
  PackedFunc lookup_linked_param_func;
  int dev_start_arg = 2;
  if (args[2].type_code() == kTVMPackedFuncHandle) {
    lookup_linked_param_func = args[2];
    dev_start_arg++;
  }
  const auto& devices = GetAllDevice(args, dev_start_arg);
  *rv = GraphExecutorCreate(args[0], args[1], devices, lookup_linked_param_func);
});
}  // namespace runtime
}  // namespace tvm
