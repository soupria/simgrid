/* Copyright (c) 2010-2018. The SimGrid Team. All rights reserved.          */

/* This program is free software; you can redistribute it and/or modify it
 * under the terms of the license (GNU LGPL) which comes with this package. */

#include "src/instr/instr_private.hpp"

#include "simgrid/kernel/routing/NetPoint.hpp"
#include "simgrid/kernel/routing/NetZoneImpl.hpp"
#include "simgrid/s4u/Actor.hpp"
#include "simgrid/s4u/Engine.hpp"
#include "simgrid/s4u/Host.hpp"
#include "src/surf/network_interface.hpp"
#include "src/surf/xml/platf_private.hpp"
#include "surf/surf.hpp"
#include "xbt/graph.h"

XBT_LOG_NEW_DEFAULT_SUBCATEGORY(instr_routing, instr, "Tracing platform hierarchy");

static std::vector<simgrid::instr::NetZoneContainer*> currentContainer; /* push and pop, used only in creation */

static const char* instr_node_name(xbt_node_t node)
{
  return static_cast<char*>(xbt_graph_node_get_data(node));
}

static container_t lowestCommonAncestor(container_t a1, container_t a2)
{
  // this is only an optimization (since most of a1 and a2 share the same parent)
  if (a1->father_ == a2->father_)
    return a1->father_;

  // create an array with all ancestors of a1
  std::vector<container_t> ancestors_a1;
  container_t p = a1->father_;
  while (p) {
    ancestors_a1.push_back(p);
    p = p->father_;
  }

  // create an array with all ancestors of a2
  std::vector<container_t> ancestors_a2;
  p = a2->father_;
  while (p) {
    ancestors_a2.push_back(p);
    p = p->father_;
  }

  // find the lowest ancestor
  p     = nullptr;
  int i = ancestors_a1.size() - 1;
  int j = ancestors_a2.size() - 1;
  while (i >= 0 && j >= 0) {
    container_t a1p = ancestors_a1.at(i);
    container_t a2p = ancestors_a2.at(j);
    if (a1p == a2p) {
      p = a1p;
    } else {
      break;
    }
    i--;
    j--;
  }
  return p;
}

static void linkContainers(container_t src, container_t dst, std::set<std::string>* filter)
{
  // ignore loopback
  if (src->get_name() == "__loopback__" || dst->get_name() == "__loopback__") {
    XBT_DEBUG("  linkContainers: ignoring loopback link");
    return;
  }

  // find common father
  container_t father = lowestCommonAncestor(src, dst);
  if (not father) {
    xbt_die("common father unknown, this is a tracing problem");
  }

  // check if we already register this pair (we only need one direction)
  std::string aux1 = src->get_name() + dst->get_name();
  std::string aux2 = dst->get_name() + src->get_name();
  if (filter->find(aux1) != filter->end()) {
    XBT_DEBUG("  linkContainers: already registered %s <-> %s (1)", src->get_cname(), dst->get_cname());
    return;
  }
  if (filter->find(aux2) != filter->end()) {
    XBT_DEBUG("  linkContainers: already registered %s <-> %s (2)", dst->get_cname(), src->get_cname());
    return;
  }

  // ok, not found, register it
  filter->insert(aux1);
  filter->insert(aux2);

  // declare type
  std::string link_typename = father->type_->get_name() + "-" + src->type_->get_name() +
                              std::to_string(src->type_->get_id()) + "-" + dst->type_->get_name() +
                              std::to_string(dst->type_->get_id());
  simgrid::instr::LinkType* link = father->type_->getOrCreateLinkType(link_typename, src->type_, dst->type_);
  link->setCallingContainer(father);

  // register EDGE types for triva configuration
  trivaEdgeTypes.insert(link->get_name());

  // create the link
  static long long counter = 0;

  std::string key = std::to_string(counter);
  counter++;

  link->startEvent(src, "topology", key);
  link->endEvent(dst, "topology", key);

  XBT_DEBUG("  linkContainers %s <-> %s", src->get_cname(), dst->get_cname());
}

static void recursiveGraphExtraction(simgrid::s4u::NetZone* netzone, container_t container,
                                     std::set<std::string>* filter)
{
  if (not TRACE_platform_topology()) {
    XBT_DEBUG("Graph extraction disabled by user.");
    return;
  }
  XBT_DEBUG("Graph extraction for NetZone = %s", netzone->get_cname());
  if (not netzone->getChildren()->empty()) {
    // bottom-up recursion
    for (auto const& nz_son : *netzone->getChildren()) {
      container_t child_container = container->children_.at(nz_son->get_cname());
      recursiveGraphExtraction(nz_son, child_container, filter);
    }
  }

  xbt_graph_t graph                        = xbt_graph_new_graph(0, nullptr);
  std::map<std::string, xbt_node_t>* nodes = new std::map<std::string, xbt_node_t>;
  std::map<std::string, xbt_edge_t>* edges = new std::map<std::string, xbt_edge_t>;

  static_cast<simgrid::kernel::routing::NetZoneImpl*>(netzone)->get_graph(graph, nodes, edges);
  for (auto elm : *edges) {
    xbt_edge_t edge = elm.second;
    linkContainers(simgrid::instr::Container::byName(static_cast<const char*>(edge->src->data)),
                   simgrid::instr::Container::byName(static_cast<const char*>(edge->dst->data)), filter);
  }
  delete nodes;
  delete edges;
  xbt_graph_free_graph(graph, xbt_free_f, xbt_free_f, nullptr);
}

/*
 * Callbacks
 */
static void instr_netzone_on_creation(simgrid::s4u::NetZone& netzone)
{
  std::string id = netzone.get_name();
  if (simgrid::instr::Container::getRoot() == nullptr) {
    simgrid::instr::NetZoneContainer* root = new simgrid::instr::NetZoneContainer(id, 0, nullptr);

    if (TRACE_smpi_is_enabled()) {
      simgrid::instr::Type* mpi = root->type_->getOrCreateContainerType("MPI");
      if (not TRACE_smpi_is_grouped())
        mpi->getOrCreateStateType("MPI_STATE");
      root->type_->getOrCreateLinkType("MPI_LINK", mpi, mpi);
      // TODO See if we can move this to the LoadBalancer plugin
      root->type_->getOrCreateLinkType("MIGRATE_LINK", mpi, mpi);
      mpi->getOrCreateStateType("MIGRATE_STATE");
    }

    if (TRACE_needs_platform()) {
      currentContainer.push_back(root);
    }
    return;
  }

  if (TRACE_needs_platform()) {
    simgrid::instr::NetZoneContainer* container =
        new simgrid::instr::NetZoneContainer(id, currentContainer.size(), currentContainer.back());
    currentContainer.push_back(container);
  }
}

static void instr_netzone_on_seal(simgrid::s4u::NetZone& /*netzone*/)
{
  if (TRACE_needs_platform()) {
    currentContainer.pop_back();
  }
}

static void instr_link_on_creation(simgrid::s4u::Link& link)
{
  if (currentContainer.empty()) // No ongoing parsing. Are you creating the loopback?
    return;

  container_t father    = currentContainer.back();
  container_t container = new simgrid::instr::Container(link.get_name(), "LINK", father);

  if ((TRACE_categorized() || TRACE_uncategorized() || TRACE_platform()) && (not TRACE_disable_link())) {
    simgrid::instr::VariableType* bandwidth = container->type_->getOrCreateVariableType("bandwidth", "");
    bandwidth->setCallingContainer(container);
    bandwidth->setEvent(0, link.bandwidth());
    simgrid::instr::VariableType* latency = container->type_->getOrCreateVariableType("latency", "");
    latency->setCallingContainer(container);
    latency->setEvent(0, link.latency());
  }
  if (TRACE_uncategorized()) {
    container->type_->getOrCreateVariableType("bandwidth_used", "0.5 0.5 0.5");
  }
}

static void instr_host_on_creation(simgrid::s4u::Host& host)
{
  container_t container = new simgrid::instr::HostContainer(host, currentContainer.back());
  container_t root      = simgrid::instr::Container::getRoot();

  if ((TRACE_categorized() || TRACE_uncategorized() || TRACE_platform()) && (not TRACE_disable_speed())) {
    simgrid::instr::VariableType* power = container->type_->getOrCreateVariableType("power", "");
    power->setCallingContainer(container);
    power->setEvent(0, host.getSpeed());
  }

  if (TRACE_uncategorized())
    container->type_->getOrCreateVariableType("power_used", "0.5 0.5 0.5");

  if (TRACE_smpi_is_enabled() && TRACE_smpi_is_grouped()) {
    simgrid::instr::ContainerType* mpi = container->type_->getOrCreateContainerType("MPI");
    mpi->getOrCreateStateType("MPI_STATE");
    // TODO See if we can move this to the LoadBalancer plugin
    root->type_->getOrCreateLinkType("MIGRATE_LINK", mpi, mpi);
    mpi->getOrCreateStateType("MIGRATE_STATE");
  }

  if (TRACE_actor_is_enabled()) {
    simgrid::instr::ContainerType* actor = container->type_->getOrCreateContainerType("ACTOR");
    simgrid::instr::StateType* state     = actor->getOrCreateStateType("ACTOR_STATE");
    state->addEntityValue("suspend", "1 0 1");
    state->addEntityValue("sleep", "1 1 0");
    state->addEntityValue("receive", "1 0 0");
    state->addEntityValue("send", "0 0 1");
    state->addEntityValue("task_execute", "0 1 1");
    root->type_->getOrCreateLinkType("ACTOR_LINK", actor, actor);
    root->type_->getOrCreateLinkType("ACTOR_TASK_LINK", actor, actor);
  }

  if (TRACE_vm_is_enabled()) {
    simgrid::instr::ContainerType* msg_vm = container->type_->getOrCreateContainerType("MSG_VM");
    simgrid::instr::StateType* state      = msg_vm->getOrCreateStateType("MSG_VM_STATE");
    state->addEntityValue("suspend", "1 0 1");
    state->addEntityValue("sleep", "1 1 0");
    state->addEntityValue("receive", "1 0 0");
    state->addEntityValue("send", "0 0 1");
    state->addEntityValue("task_execute", "0 1 1");
    root->type_->getOrCreateLinkType("MSG_VM_LINK", msg_vm, msg_vm);
    root->type_->getOrCreateLinkType("MSG_VM_ACTOR_LINK", msg_vm, msg_vm);
  }
}

static void instr_netpoint_on_creation(simgrid::kernel::routing::NetPoint* netpoint)
{
  if (netpoint->is_router() && TRACE_needs_platform() && TRACE_is_enabled())
    new simgrid::instr::RouterContainer(netpoint->get_cname(), currentContainer.back());
}

static void instr_on_platform_created()
{
  currentContainer.clear();
  std::set<std::string>* filter = new std::set<std::string>;
  XBT_DEBUG("Starting graph extraction.");
  recursiveGraphExtraction(simgrid::s4u::Engine::get_instance()->getNetRoot(), simgrid::instr::Container::getRoot(),
                           filter);
  XBT_DEBUG("Graph extraction finished.");
  delete filter;
  TRACE_paje_dump_buffer(true);
}

static void instr_on_actor_suspend(simgrid::s4u::ActorPtr actor)
{
  simgrid::instr::Container::byName(instr_pid(actor.get()))->getState("ACTOR_STATE")->pushEvent("suspend");
}

static void instr_on_actor_resume(simgrid::s4u::ActorPtr actor)
{
  simgrid::instr::Container::byName(instr_pid(actor.get()))->getState("ACTOR_STATE")->popEvent();
}

static long long int counter = 0;

static void instr_on_actor_migration_start(simgrid::s4u::ActorPtr actor)
{
  // start link
  container_t container = simgrid::instr::Container::byName(instr_pid(actor.get()));
  simgrid::instr::Container::getRoot()->getLink("ACTOR_LINK")->startEvent(container, "M", std::to_string(counter));

  // destroy existing container of this process
  container->removeFromParent();
}

static void instr_on_actor_migration_end(simgrid::s4u::ActorPtr actor)
{
  // create new container on the new_host location
  simgrid::instr::Container::byName(actor->get_host()->get_name())->createChild(instr_pid(actor.get()), "ACTOR");
  // end link
  simgrid::instr::Container::getRoot()
      ->getLink("ACTOR_LINK")
      ->endEvent(simgrid::instr::Container::byName(instr_pid(actor.get())), "M", std::to_string(counter));
  counter++;
}

void instr_define_callbacks()
{
  // always need the callbacks to zones (we need only the root zone), to create the rootContainer and the rootType
  // properly
  if (TRACE_needs_platform()) {
    simgrid::s4u::on_platform_created.connect(instr_on_platform_created);
    simgrid::s4u::Host::onCreation.connect(instr_host_on_creation);
    simgrid::s4u::Link::onCreation.connect(instr_link_on_creation);
  }
  simgrid::s4u::NetZone::onCreation.connect(instr_netzone_on_creation);
  simgrid::s4u::NetZone::onSeal.connect(instr_netzone_on_seal);
  simgrid::kernel::routing::NetPoint::onCreation.connect(instr_netpoint_on_creation);
  if (TRACE_actor_is_enabled()) {
    simgrid::s4u::Actor::on_suspend.connect(instr_on_actor_suspend);
    simgrid::s4u::Actor::on_resume.connect(instr_on_actor_resume);
    simgrid::s4u::Actor::on_migration_start.connect(instr_on_actor_migration_start);
    simgrid::s4u::Actor::on_migration_end.connect(instr_on_actor_migration_end);
  }
}
/*
 * user categories support
 */
static void recursiveNewVariableType(std::string new_typename, std::string color, simgrid::instr::Type* root)
{
  if (root->get_name() == "HOST" || root->get_name() == "MSG_VM")
    root->getOrCreateVariableType(std::string("p") + new_typename, color);

  if (root->get_name() == "LINK")
    root->getOrCreateVariableType(std::string("b") + new_typename, color);

  for (auto elm : root->children_) {
    recursiveNewVariableType(new_typename, color, elm.second);
  }
}

void instr_new_variable_type(std::string new_typename, std::string color)
{
  recursiveNewVariableType(new_typename, color, simgrid::instr::Container::getRoot()->type_);
}

static void recursiveNewUserVariableType(std::string father_type, std::string new_typename, std::string color,
                                         simgrid::instr::Type* root)
{
  if (root->get_name() == father_type) {
    root->getOrCreateVariableType(new_typename, color);
  }
  for (auto elm : root->children_)
    recursiveNewUserVariableType(father_type, new_typename, color, elm.second);
}

void instr_new_user_variable_type(std::string father_type, std::string new_typename, std::string color)
{
  recursiveNewUserVariableType(father_type, new_typename, color, simgrid::instr::Container::getRoot()->type_);
}

static void recursiveNewUserStateType(std::string father_type, std::string new_typename, simgrid::instr::Type* root)
{
  if (root->get_name() == father_type)
    root->getOrCreateStateType(new_typename);

  for (auto elm : root->children_)
    recursiveNewUserStateType(father_type, new_typename, elm.second);
}

void instr_new_user_state_type(std::string father_type, std::string new_typename)
{
  recursiveNewUserStateType(father_type, new_typename, simgrid::instr::Container::getRoot()->type_);
}

static void recursiveNewValueForUserStateType(std::string type_name, const char* val, std::string color,
                                              simgrid::instr::Type* root)
{
  if (root->get_name() == type_name)
    static_cast<simgrid::instr::StateType*>(root)->addEntityValue(val, color);

  for (auto elm : root->children_)
    recursiveNewValueForUserStateType(type_name, val, color, elm.second);
}

void instr_new_value_for_user_state_type(std::string type_name, const char* value, std::string color)
{
  recursiveNewValueForUserStateType(type_name, value, color, simgrid::instr::Container::getRoot()->type_);
}

#define GRAPHICATOR_SUPPORT_FUNCTIONS

static void recursiveXBTGraphExtraction(xbt_graph_t graph, std::map<std::string, xbt_node_t>* nodes,
                                        std::map<std::string, xbt_edge_t>* edges, sg_netzone_t netzone,
                                        container_t container)
{
  if (not netzone->getChildren()->empty()) {
    // bottom-up recursion
    for (auto const& netzone_child : *netzone->getChildren()) {
      container_t child_container = container->children_.at(netzone_child->get_cname());
      recursiveXBTGraphExtraction(graph, nodes, edges, netzone_child, child_container);
    }
  }

  static_cast<simgrid::kernel::routing::NetZoneImpl*>(netzone)->get_graph(graph, nodes, edges);
}

xbt_graph_t instr_routing_platform_graph()
{
  xbt_graph_t ret                          = xbt_graph_new_graph(0, nullptr);
  std::map<std::string, xbt_node_t>* nodes = new std::map<std::string, xbt_node_t>;
  std::map<std::string, xbt_edge_t>* edges = new std::map<std::string, xbt_edge_t>;
  recursiveXBTGraphExtraction(ret, nodes, edges, simgrid::s4u::Engine::get_instance()->getNetRoot(),
                              simgrid::instr::Container::getRoot());
  delete nodes;
  delete edges;
  return ret;
}

void instr_routing_platform_graph_export_graphviz(xbt_graph_t g, const char* filename)
{
  unsigned int cursor = 0;
  xbt_node_t node     = nullptr;
  xbt_edge_t edge     = nullptr;

  FILE* file = fopen(filename, "w");
  xbt_assert(file, "Failed to open %s \n", filename);

  if (g->directed)
    fprintf(file, "digraph test {\n");
  else
    fprintf(file, "graph test {\n");

  fprintf(file, "  graph [overlap=scale]\n");

  fprintf(file, "  node [shape=box, style=filled]\n");
  fprintf(file, "  node [width=.3, height=.3, style=filled, color=skyblue]\n\n");

  xbt_dynar_foreach (g->nodes, cursor, node) {
    fprintf(file, "  \"%s\";\n", instr_node_name(node));
  }
  xbt_dynar_foreach (g->edges, cursor, edge) {
    const char* src_s = instr_node_name(edge->src);
    const char* dst_s = instr_node_name(edge->dst);
    if (g->directed)
      fprintf(file, "  \"%s\" -> \"%s\";\n", src_s, dst_s);
    else
      fprintf(file, "  \"%s\" -- \"%s\";\n", src_s, dst_s);
  }
  fprintf(file, "}\n");
  fclose(file);
}