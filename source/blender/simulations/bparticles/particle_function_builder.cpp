#include "FN_llvm.hpp"

#include "particle_function_builder.hpp"

#include "events.hpp"

namespace BParticles {

using BKE::VirtualSocket;
using FN::DataSocket;
using FN::FunctionBuilder;
using FN::FunctionGraph;
using FN::SharedDataGraph;
using FN::SharedFunction;
using FN::SharedType;

Vector<DataSocket> find_input_data_sockets(VirtualNode *vnode, VTreeDataGraph &data_graph)
{
  Vector<DataSocket> inputs;
  for (VirtualSocket *vsocket : vnode->inputs()) {
    DataSocket *socket = data_graph.lookup_socket_ptr(vsocket);
    if (socket != nullptr) {
      inputs.append(*socket);
    }
  }
  return inputs;
}

struct SocketDependencies {
  SetVector<DataSocket> sockets;
  SetVector<VirtualSocket *> vsockets;
};

static SocketDependencies find_particle_dependencies(VTreeDataGraph &data_graph,
                                                     ArrayRef<DataSocket> sockets,
                                                     ArrayRef<bool> r_depends_on_particle_flags)
{
  SocketDependencies combined_dependencies;

  for (uint i = 0; i < sockets.size(); i++) {
    DataSocket socket = sockets[i];
    auto dependencies = data_graph.find_placeholder_dependencies(socket);
    bool has_dependency = dependencies.size() > 0;
    r_depends_on_particle_flags[i] = has_dependency;

    combined_dependencies.sockets.add_multiple(dependencies.sockets);
    combined_dependencies.vsockets.add_multiple(dependencies.vsockets);
    BLI_assert(combined_dependencies.sockets.size() == combined_dependencies.vsockets.size());
  }

  return combined_dependencies;
}

class AttributeInputProvider : public ParticleFunctionInputProvider {
 private:
  std::string m_name;

 public:
  AttributeInputProvider(StringRef name) : m_name(name.to_std_string())
  {
  }

  ParticleFunctionInputArray get(InputProviderInterface &interface) override
  {
    AttributeArrays attributes = interface.particles().attributes();
    uint attribute_index = attributes.attribute_index(m_name);
    uint stride = attributes.attribute_stride(attribute_index);
    void *buffer = attributes.get_ptr(attribute_index);
    return {buffer, stride, false};
  }
};

class CollisionNormalInputProvider : public ParticleFunctionInputProvider {
  ParticleFunctionInputArray get(InputProviderInterface &interface) override
  {
    ActionContext *action_context = interface.action_context();
    BLI_assert(action_context != nullptr);
    CollisionEventInfo *collision_info = dynamic_cast<CollisionEventInfo *>(action_context);
    BLI_assert(collision_info != nullptr);
    return {collision_info->normals(), false};
  }
};

class AgeInputProvider : public ParticleFunctionInputProvider {
  ParticleFunctionInputArray get(InputProviderInterface &interface) override
  {
    auto birth_times = interface.particles().attributes().get<float>("Birth Time");
    float *ages = interface.array_allocator().allocate<float>();

    ParticleTimes &times = interface.particle_times();
    if (times.type() == ParticleTimes::Type::Current) {
      auto current_times = times.current_times();
      for (uint pindex : interface.particles().pindices()) {
        ages[pindex] = current_times[pindex] - birth_times[pindex];
      }
    }
    else if (times.type() == ParticleTimes::Type::DurationAndEnd) {
      auto remaining_durations = times.remaining_durations();
      float end_time = times.end_time();
      for (uint pindex : interface.particles().pindices()) {
        ages[pindex] = end_time - remaining_durations[pindex] - birth_times[pindex];
      }
    }
    else {
      BLI_assert(false);
    }
    return {ArrayRef<float>(ages, interface.array_allocator().array_size()), true};
  }
};

static ParticleFunctionInputProvider *create_input_provider(VirtualSocket *vsocket)
{
  VirtualNode *vnode = vsocket->vnode();
  if (STREQ(vnode->idname(), "bp_ParticleInfoNode")) {
    if (STREQ(vsocket->name(), "Age")) {
      return new AgeInputProvider();
    }
    else {
      return new AttributeInputProvider(vsocket->name());
    }
  }
  else if (STREQ(vnode->idname(), "bp_CollisionInfoNode")) {
    return new CollisionNormalInputProvider();
  }
  else {
    BLI_assert(false);
    return nullptr;
  }
}

static SharedFunction create_function__with_deps(
    SharedDataGraph &graph,
    StringRef function_name,
    ArrayRef<DataSocket> sockets_to_compute,
    SocketDependencies &dependencies,
    ArrayRef<ParticleFunctionInputProvider *> r_input_providers)
{
  uint input_amount = dependencies.sockets.size();
  BLI_assert(input_amount == r_input_providers.size());

  FunctionBuilder fn_builder;
  fn_builder.add_inputs(graph, dependencies.sockets);
  fn_builder.add_outputs(graph, sockets_to_compute);

  for (uint i = 0; i < input_amount; i++) {
    VirtualSocket *vsocket = dependencies.vsockets[i];
    r_input_providers[i] = create_input_provider(vsocket);
  }

  SharedFunction fn = fn_builder.build(function_name);
  FunctionGraph fgraph(graph, dependencies.sockets, sockets_to_compute);
  FN::fgraph_add_TupleCallBody(fn, fgraph);
  FN::fgraph_add_LLVMBuildIRBody(fn, fgraph);
  return fn;
}

static SharedFunction create_function__without_deps(SharedDataGraph &graph,
                                                    StringRef function_name,
                                                    ArrayRef<DataSocket> sockets_to_compute)
{
  FunctionBuilder fn_builder;
  fn_builder.add_outputs(graph, sockets_to_compute);
  SharedFunction fn = fn_builder.build(function_name);
  FunctionGraph fgraph(graph, {}, sockets_to_compute);
  FN::fgraph_add_TupleCallBody(fn, fgraph);
  return fn;
}

static ValueOrError<std::unique_ptr<ParticleFunction>> create_particle_function_from_sockets(
    SharedDataGraph &graph,
    StringRef name,
    ArrayRef<DataSocket> sockets_to_compute,
    ArrayRef<bool> depends_on_particle_flags,
    SocketDependencies &dependencies)
{
  Vector<DataSocket> sockets_with_deps;
  Vector<DataSocket> sockets_without_deps;
  for (uint i = 0; i < sockets_to_compute.size(); i++) {
    if (depends_on_particle_flags[i]) {
      sockets_with_deps.append(sockets_to_compute[i]);
    }
    else {
      sockets_without_deps.append(sockets_to_compute[i]);
    }
  }

  Vector<ParticleFunctionInputProvider *> input_providers(dependencies.sockets.size(), nullptr);

  SharedFunction fn_without_deps = create_function__without_deps(
      graph, name, sockets_without_deps);
  SharedFunction fn_with_deps = create_function__with_deps(
      graph, name, sockets_with_deps, dependencies, input_providers);

  ParticleFunction *particle_fn = new ParticleFunction(
      fn_without_deps, fn_with_deps, input_providers, depends_on_particle_flags);
  return std::unique_ptr<ParticleFunction>(particle_fn);
}

ValueOrError<std::unique_ptr<ParticleFunction>> create_particle_function(
    VirtualNode *vnode, VTreeDataGraph &data_graph)
{
  Vector<DataSocket> sockets_to_compute = find_input_data_sockets(vnode, data_graph);
  Vector<bool> depends_on_particle_flags(sockets_to_compute.size());
  auto dependencies = find_particle_dependencies(
      data_graph, sockets_to_compute, depends_on_particle_flags);

  std::string name = vnode->name() + StringRef(" Inputs");
  return create_particle_function_from_sockets(
      data_graph.graph(), name, sockets_to_compute, depends_on_particle_flags, dependencies);
}

}  // namespace BParticles