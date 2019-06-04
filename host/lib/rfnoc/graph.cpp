//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#include <uhd/exception.hpp>
#include <uhd/utils/log.hpp>
#include <uhdlib/rfnoc/graph.hpp>
#include <uhdlib/rfnoc/node_accessor.hpp>
#include <boost/graph/topological_sort.hpp>
#include <boost/graph/filtered_graph.hpp>
#include <utility>

using namespace uhd::rfnoc;
using namespace uhd::rfnoc::detail;

namespace {

const std::string LOG_ID = "RFNOC::GRAPH::DETAIL";
constexpr unsigned MAX_ACTION_ITERATIONS = 200;

/*! Helper function to pretty-print edge info
 */
std::string print_edge(
    graph_t::node_ref_t src, graph_t::node_ref_t dst, graph_t::graph_edge_t edge_info)
{
    return src->get_unique_id() + ":" + std::to_string(edge_info.src_port) + " -> "
           + dst->get_unique_id() + ":" + std::to_string(edge_info.dst_port);
}

/*! Return a list of dirty properties from a node
 */
auto get_dirty_props(graph_t::node_ref_t node_ref)
{
    using namespace uhd::rfnoc;
    node_accessor_t node_accessor{};
    return node_accessor.filter_props(node_ref, [](property_base_t* prop) {
        return prop->is_dirty()
               && prop->get_src_info().type != res_source_info::FRAMEWORK;
    });
}

/*! Check that \p new_edge_info does not conflict with \p existing_edge_info
 *
 * \throws uhd::rfnoc_error if it does.
 */
void assert_edge_new(const graph_t::graph_edge_t& new_edge_info,
    const graph_t::graph_edge_t& existing_edge_info)
{
    if (existing_edge_info == new_edge_info) {
        UHD_LOG_INFO(LOG_ID,
            "Ignoring repeated call to connect "
                << new_edge_info.src_blockid << ":" << new_edge_info.src_port << " -> "
                << new_edge_info.dst_blockid << ":" << new_edge_info.dst_port);
        return;
    } else if (existing_edge_info.src_port == new_edge_info.src_port
               && existing_edge_info.src_blockid == new_edge_info.src_blockid
               && existing_edge_info.dst_port == new_edge_info.dst_port
               && existing_edge_info.dst_blockid == new_edge_info.dst_blockid) {
        UHD_LOG_ERROR(LOG_ID,
            "Caught attempt to modify properties of edge "
                << existing_edge_info.src_blockid << ":" << existing_edge_info.src_port
                << " -> " << existing_edge_info.dst_blockid << ":"
                << existing_edge_info.dst_port);
        throw uhd::rfnoc_error("Caught attempt to modify properties of edge!");
    } else if (new_edge_info.src_blockid == existing_edge_info.src_blockid
               && new_edge_info.src_port == existing_edge_info.src_port) {
        UHD_LOG_ERROR(LOG_ID,
            "Attempting to reconnect output port " << existing_edge_info.src_blockid
                                                   << ":" << existing_edge_info.src_port);
        throw uhd::rfnoc_error("Attempting to reconnect output port!");
    } else if (new_edge_info.dst_blockid == existing_edge_info.dst_blockid
               && new_edge_info.dst_port == existing_edge_info.dst_port) {
        UHD_LOG_ERROR(LOG_ID,
            "Attempting to reconnect output port " << existing_edge_info.dst_blockid
                                                   << ":" << existing_edge_info.dst_port);
        throw uhd::rfnoc_error("Attempting to reconnect input port!");
    }
}

} // namespace

/*! Graph-filtering predicate to find dirty nodes only
 */
struct graph_t::DirtyNodePredicate
{
    DirtyNodePredicate() {} // Default ctor is required
    DirtyNodePredicate(graph_t::rfnoc_graph_t& graph) : _graph(&graph) {}

    template <typename Vertex>
    bool operator()(const Vertex& v) const
    {
        return !get_dirty_props(boost::get(graph_t::vertex_property_t(), *_graph, v))
                    .empty();
    }

private:
    // Don't make any attribute const, because default assignment operator
    // is also required
    graph_t::rfnoc_graph_t* _graph;
};

/******************************************************************************
 * Public API calls
 *****************************************************************************/
void graph_t::connect(node_ref_t src_node, node_ref_t dst_node, graph_edge_t edge_info)
{
    node_accessor_t node_accessor{};
    UHD_LOG_TRACE(LOG_ID,
        "Connecting block " << src_node->get_unique_id() << ":" << edge_info.src_port
                            << " -> " << dst_node->get_unique_id() << ":"
                            << edge_info.dst_port);

    // Correctly populate edge_info
    edge_info.src_blockid = src_node->get_unique_id();
    edge_info.dst_blockid = dst_node->get_unique_id();

    // Set resolver callbacks:
    node_accessor.set_resolve_all_callback(
        src_node, [this]() { this->resolve_all_properties(); });
    node_accessor.set_resolve_all_callback(
        dst_node, [this]() { this->resolve_all_properties(); });
    // Set post action callbacks:
    node_accessor.set_post_action_callback(
        src_node, [this, src_node](const res_source_info& src, action_info::sptr action) {
            this->enqueue_action(src_node, src, action);
        });
    node_accessor.set_post_action_callback(
        dst_node, [this, dst_node](const res_source_info& src, action_info::sptr action) {
            this->enqueue_action(dst_node, src, action);
        });

    // Add nodes to graph, if not already in there:
    _add_node(src_node);
    _add_node(dst_node);
    // Find vertex descriptors
    auto src_vertex_desc = _node_map.at(src_node);
    auto dst_vertex_desc = _node_map.at(dst_node);

    // Check if connection exists
    // This can be optimized: Edges can appear in both out_edges and in_edges,
    // and we could skip double-checking them.
    auto out_edge_range = boost::out_edges(src_vertex_desc, _graph);
    for (auto edge_it = out_edge_range.first; edge_it != out_edge_range.second;
         ++edge_it) {
        assert_edge_new(edge_info, boost::get(edge_property_t(), _graph, *edge_it));
    }
    auto in_edge_range = boost::in_edges(dst_vertex_desc, _graph);
    for (auto edge_it = in_edge_range.first; edge_it != in_edge_range.second; ++edge_it) {
        assert_edge_new(edge_info, boost::get(edge_property_t(), _graph, *edge_it));
    }

    // Create edge
    auto edge_descriptor =
        boost::add_edge(src_vertex_desc, dst_vertex_desc, edge_info, _graph);
    UHD_ASSERT_THROW(edge_descriptor.second);

    // Now make sure we didn't add an unintended cycle
    try {
        _get_topo_sorted_nodes();
    } catch (const uhd::rfnoc_error&) {
        UHD_LOG_ERROR(LOG_ID,
            "Adding edge " << src_node->get_unique_id() << ":" << edge_info.src_port
                           << " -> " << dst_node->get_unique_id() << ":"
                           << edge_info.dst_port
                           << " without disabling property_propagation_active will lead "
                              "to unresolvable graph!");
        boost::remove_edge(edge_descriptor.first, _graph);
        throw uhd::rfnoc_error(
            "Adding edge without disabling property_propagation_active will lead "
            "to unresolvable graph!");
    }
}

void graph_t::commit()
{
    if (_release_count) {
        _release_count--;
        _check_topology();
    }
    UHD_LOG_TRACE(LOG_ID, "graph::commit() => " << _release_count.load());
    resolve_all_properties();
}

void graph_t::release()
{
    UHD_LOG_TRACE(LOG_ID, "graph::release() => " << _release_count.load());
    _release_count++;
}


/******************************************************************************
 * Private methods to be called by friends
 *****************************************************************************/
void graph_t::resolve_all_properties()
{
    if (boost::num_vertices(_graph) == 0) {
        return;
    }
    if (_release_count) {
        return;
    }
    node_accessor_t node_accessor{};

    // First, find the node on which we'll start.
    auto initial_dirty_nodes = _find_dirty_nodes();
    if (initial_dirty_nodes.size() > 1) {
        UHD_LOGGER_WARNING(LOG_ID)
            << "Found " << initial_dirty_nodes.size()
            << " dirty nodes in initial search (expected one or zero). "
               "Property propagation may resolve this.";
        for (auto& vertex : initial_dirty_nodes) {
            node_ref_t node = boost::get(vertex_property_t(), _graph, vertex);
            UHD_LOG_WARNING(LOG_ID, "Dirty: " << node->get_unique_id());
        }
    }
    if (initial_dirty_nodes.empty()) {
        UHD_LOG_DEBUG(LOG_ID,
            "In resolve_all_properties(): No dirty properties found. Starting on "
            "arbitrary node.");
        initial_dirty_nodes.push_back(*boost::vertices(_graph).first);
    }
    UHD_ASSERT_THROW(!initial_dirty_nodes.empty());
    auto initial_node = initial_dirty_nodes.front();

    // Now get all nodes in topologically sorted order, and the appropriate
    // iterators.
    auto topo_sorted_nodes = _get_topo_sorted_nodes();
    auto node_it           = topo_sorted_nodes.begin();
    auto begin_it          = topo_sorted_nodes.begin();
    auto end_it            = topo_sorted_nodes.end();
    while (*node_it != initial_node) {
        // We know *node_it must be == initial_node at some point, because
        // otherwise, initial_dirty_nodes would have been empty
        node_it++;
    }

    // Start iterating over nodes
    bool forward_dir                 = true;
    int num_iterations               = 0;
    // If all edge properties were known at the beginning, a single iteration
    // would suffice. However, usually during the first time the property
    // propagation is run, blocks create new (dynamic) edge properties that
    // default to dirty. If we had a way of knowing when that happens, we could
    // dynamically increase the number of iterations during the loop. For now,
    // we simply hard-code the number of iterations to 2 so that we catch that
    // case without any additional complications.
    constexpr int MAX_NUM_ITERATIONS = 2;
    while (true) {
        node_ref_t current_node = boost::get(vertex_property_t(), _graph, *node_it);
        UHD_LOG_TRACE(
            LOG_ID, "Now resolving next node: " << current_node->get_unique_id());

        // On current node, call local resolution. This may cause other
        // properties to become dirty.
        node_accessor.resolve_props(current_node);

        //  Forward all edge props in all directions from current node. We make
        //  sure to skip properties if the edge is flagged as
        //  !property_propagation_active
        _forward_edge_props(*node_it);

        // Now mark all properties on this node as clean
        node_accessor.clean_props(current_node);

        // The rest of the code in this loop is to figure out who's the next
        // node. First, increment (or decrement) iterator:
        if (forward_dir) {
            node_it++;
            // If we're at the end, flip the direction
            if (node_it == end_it) {
                forward_dir = false;
                // Back off from the sentinel:
                node_it--;
            }
        }
        if (!forward_dir) {
            if (topo_sorted_nodes.size() > 1) {
                node_it--;
                // If we're back at the front, flip direction
                if (node_it == begin_it) {
                    forward_dir = true;
                }
            } else {
                forward_dir = true;
            }
        }
        // If we're going forward, and the next node is the initial node,
        // we've gone full circle (one full iteration).
        if (forward_dir && (*node_it == initial_node)) {
            num_iterations++;
            if (num_iterations == MAX_NUM_ITERATIONS) {
                UHD_LOG_TRACE(LOG_ID,
                    "Terminating graph resolution after iteration " << num_iterations);
                break;
            }
        }
    }

    // Post-iteration sanity checks:
    // First, we make sure that there are no dirty properties left. If there are,
    // that means our algorithm couldn't converge and we have a problem.
    auto remaining_dirty_nodes = _find_dirty_nodes();
    if (!remaining_dirty_nodes.empty()) {
        UHD_LOG_ERROR(LOG_ID, "The following properties could not be resolved:");
        for (auto& vertex : remaining_dirty_nodes) {
            node_ref_t node           = boost::get(vertex_property_t(), _graph, vertex);
            const std::string node_id = node->get_unique_id();
            auto dirty_props          = get_dirty_props(node);
            for (auto& prop : dirty_props) {
                UHD_LOG_ERROR(LOG_ID,
                    "Dirty: " << node_id << "[" << prop->get_src_info().to_string() << " "
                              << prop->get_id() << "]");
            }
        }
        throw uhd::resolve_error("Could not resolve properties.");
    }

    // Second, go through edges marked !property_propagation_active and make
    // sure that they match up
    BackEdgePredicate back_edge_filter(_graph);
    auto e_iterators =
        boost::edges(boost::filtered_graph<rfnoc_graph_t, BackEdgePredicate>(
            _graph, back_edge_filter));
    bool back_edges_valid = true;
    for (auto e_it = e_iterators.first; e_it != e_iterators.second; ++e_it) {
        back_edges_valid = back_edges_valid && _assert_edge_props_consistent(*e_it);
    }
    if (!back_edges_valid) {
        throw uhd::resolve_error(
            "Error during property resultion: Back-edges inconsistent!");
    }
}

void graph_t::enqueue_action(
    node_ref_t src_node, res_source_info src_edge, action_info::sptr action)
{
    if (_release_count) {
        UHD_LOG_WARNING(LOG_ID,
            "Action propagation is not enabled, graph is not committed! Will not "
            "propagate action `"
                << action->key << "'");
        return;
    }
    // First, make sure that once we start action handling, no other node from
    // a different thread can throw in their own actions
    std::lock_guard<std::recursive_mutex> l(_action_mutex);

    // Check if we're already in the middle of handling actions. In that case,
    // we're already in the loop below, and then all we want to do is to enqueue
    // this action tuple. The first call to enqueue_action() within this thread
    // context will have handling_ongoing == false.
    const bool handling_ongoing = _action_handling_ongoing.test_and_set();

    _action_queue.emplace_back(std::make_tuple(src_node, src_edge, action));
    if (handling_ongoing) {
        UHD_LOG_TRACE(LOG_ID,
            "Action handling ongoing, deferring delivery of " << action->key << "#"
                                                              << action->id);
        return;
    }

    unsigned iteration_count = 0;
    while (!_action_queue.empty()) {
        if (iteration_count++ == MAX_ACTION_ITERATIONS) {
            throw uhd::runtime_error("Terminating action handling: Reached "
                                     "recursion limit!");
        }

        // Unpack next action
        auto& next_action               = _action_queue.front();
        node_ref_t action_src_node      = std::get<0>(next_action);
        res_source_info action_src_port = std::get<1>(next_action);
        action_info::sptr next_action_sptr   = std::get<2>(next_action);
        _action_queue.pop_front();

        // Find the node that is supposed to receive this action, and if we find
        // something, then send the action
        auto recipient_info =
            _find_neighbour(_node_map.at(action_src_node), action_src_port);
        if (recipient_info.first == nullptr) {
            UHD_LOG_WARNING(LOG_ID,
                "Cannot forward action "
                    << action->key << " from " << src_node->get_unique_id()
                    << ":" << src_edge.to_string() << ", no neighbour found!");
        } else {
            node_ref_t recipient_node      = recipient_info.first;
            res_source_info recipient_port = {
                res_source_info::invert_edge(action_src_port.type),
                action_src_port.type == res_source_info::INPUT_EDGE
                    ? recipient_info.second.dst_port
                    : recipient_info.second.src_port};
            // The following call can cause other nodes to add more actions to
            // the end of _action_queue!
            UHD_LOG_TRACE(LOG_ID,
                "Now delivering action " << next_action_sptr->key << "#"
                                         << next_action_sptr->id);
            node_accessor_t{}.send_action(
                recipient_node, recipient_port, next_action_sptr);
        }
    }
    UHD_LOG_TRACE(LOG_ID, "Delivered all actions, terminating action handling.");

    // Release the action handling flag
    _action_handling_ongoing.clear();
    // Now, the _action_mutex is released, and someone else can start sending
    // actions.
}

/******************************************************************************
 * Private methods
 *****************************************************************************/
graph_t::vertex_list_t graph_t::_find_dirty_nodes()
{
    // Create a view on the graph that doesn't include the back-edges
    DirtyNodePredicate vertex_filter(_graph);
    boost::filtered_graph<rfnoc_graph_t, boost::keep_all, DirtyNodePredicate> fg(
        _graph, boost::keep_all(), vertex_filter);

    auto v_iterators = boost::vertices(fg);
    return vertex_list_t(v_iterators.first, v_iterators.second);
}

graph_t::vertex_list_t graph_t::_get_topo_sorted_nodes()
{
    // Create a view on the graph that doesn't include the back-edges
    ForwardEdgePredicate edge_filter(_graph);
    boost::filtered_graph<rfnoc_graph_t, ForwardEdgePredicate> fg(_graph, edge_filter);

    // Topo-sort and return
    vertex_list_t sorted_nodes;
    try {
        boost::topological_sort(fg, std::front_inserter(sorted_nodes));
    } catch (boost::not_a_dag&) {
        throw uhd::rfnoc_error("Cannot resolve graph because it has at least one cycle!");
    }
    return sorted_nodes;
}

void graph_t::_add_node(node_ref_t new_node)
{
    if (_node_map.count(new_node)) {
        return;
    }

    _node_map.emplace(new_node, boost::add_vertex(new_node, _graph));
}


void graph_t::_forward_edge_props(graph_t::rfnoc_graph_t::vertex_descriptor origin)
{
    node_accessor_t node_accessor{};
    node_ref_t origin_node = boost::get(vertex_property_t(), _graph, origin);

    auto edge_props = node_accessor.filter_props(origin_node, [](property_base_t* prop) {
        return (prop->get_src_info().type == res_source_info::INPUT_EDGE
                || prop->get_src_info().type == res_source_info::OUTPUT_EDGE);
    });
    UHD_LOG_TRACE(LOG_ID,
        "Forwarding up to " << edge_props.size() << " edge properties from node "
                            << origin_node->get_unique_id());

    for (auto prop : edge_props) {
        auto neighbour_node_info = _find_neighbour(origin, prop->get_src_info());
        if (neighbour_node_info.first != nullptr
            && neighbour_node_info.second.property_propagation_active) {
            const size_t neighbour_port = prop->get_src_info().type
                                                  == res_source_info::INPUT_EDGE
                                              ? neighbour_node_info.second.src_port
                                              : neighbour_node_info.second.dst_port;
            node_accessor.forward_edge_property(
                neighbour_node_info.first, neighbour_port, prop);
        }
    }
}

bool graph_t::_assert_edge_props_consistent(rfnoc_graph_t::edge_descriptor edge)
{
    node_ref_t src_node =
        boost::get(vertex_property_t(), _graph, boost::source(edge, _graph));
    node_ref_t dst_node =
        boost::get(vertex_property_t(), _graph, boost::target(edge, _graph));
    graph_edge_t edge_info = boost::get(edge_property_t(), _graph, edge);

    // Helper function to get properties as maps
    auto get_prop_map = [](const size_t port,
                            res_source_info::source_t edge_type,
                            node_ref_t node) {
        node_accessor_t node_accessor{};
        // Create a set of all properties
        auto props_set = node_accessor.filter_props(
            node, [port, edge_type, node](property_base_t* prop) {
                return prop->get_src_info().instance == port
                       && prop->get_src_info().type == edge_type;
            });
        std::unordered_map<std::string, property_base_t*> prop_map;
        for (auto prop_it = props_set.begin(); prop_it != props_set.end(); ++prop_it) {
            prop_map.emplace((*prop_it)->get_id(), *prop_it);
        }

        return prop_map;
    };

    // Create two maps ID -> prop_ptr, so we have an easier time comparing them
    auto src_prop_map =
        get_prop_map(edge_info.src_port, res_source_info::OUTPUT_EDGE, src_node);
    auto dst_prop_map =
        get_prop_map(edge_info.dst_port, res_source_info::INPUT_EDGE, dst_node);

    // Now iterate through all properties, and make sure they match
    bool props_match = true;
    for (auto src_prop_it = src_prop_map.begin(); src_prop_it != src_prop_map.end();
         ++src_prop_it) {
        auto src_prop = src_prop_it->second;
        auto dst_prop = dst_prop_map.at(src_prop->get_id());
        if (!src_prop->equal(dst_prop)) {
            UHD_LOG_ERROR(LOG_ID,
                "Edge property " << src_prop->get_id() << " inconsistent on edge "
                                 << print_edge(src_node, dst_node, edge_info));
            props_match = false;
        }
    }

    return props_match;
}

void graph_t::_check_topology()
{
    node_accessor_t node_accessor{};
    bool topo_ok = true;
    auto v_iterators = boost::vertices(_graph);
    for (auto it = v_iterators.first; it != v_iterators.second; ++it) {
        node_ref_t node = boost::get(vertex_property_t(), _graph, *it);
        std::vector<size_t> connected_inputs;
        std::vector<size_t> connected_outputs;
        auto ie_iters = boost::in_edges(*it, _graph);
        for (auto it = ie_iters.first; it != ie_iters.second; ++it) {
            graph_edge_t edge_info = boost::get(edge_property_t(), _graph, *it);
            connected_inputs.push_back(edge_info.dst_port);
        }
        auto oe_iters = boost::out_edges(*it, _graph);
        for (auto it = oe_iters.first; it != oe_iters.second; ++it) {
            graph_edge_t edge_info = boost::get(edge_property_t(), _graph, *it);
            connected_outputs.push_back(edge_info.src_port);
        }

        if (!node_accessor.check_topology(node, connected_inputs, connected_outputs)) {
            UHD_LOG_ERROR(LOG_ID,
                "Node " << node->get_unique_id()
                        << "cannot handle its current topology! ("
                        << connected_inputs.size() << "inputs, "
                        << connected_outputs.size() << " outputs)");
            topo_ok = false;
        }
    }

    if (!topo_ok) {
        throw uhd::runtime_error("Graph topology is not valid!");
    }
}

std::pair<graph_t::node_ref_t, graph_t::graph_edge_t> graph_t::_find_neighbour(
    rfnoc_graph_t::vertex_descriptor origin, res_source_info port_info)
{
    if (port_info.type == res_source_info::INPUT_EDGE) {
        auto it_range = boost::in_edges(origin, _graph);
        for (auto it = it_range.first; it != it_range.second; ++it) {
            graph_edge_t edge_info = boost::get(edge_property_t(), _graph, *it);
            if (edge_info.dst_port == port_info.instance) {
                return {
                    boost::get(vertex_property_t(), _graph, boost::source(*it, _graph)),
                    edge_info};
            }
        }
        return {nullptr, {}};
    }
    if (port_info.type == res_source_info::OUTPUT_EDGE) {
        auto it_range = boost::out_edges(origin, _graph);
        for (auto it = it_range.first; it != it_range.second; ++it) {
            graph_edge_t edge_info = boost::get(edge_property_t(), _graph, *it);
            if (edge_info.src_port == port_info.instance) {
                return {
                    boost::get(vertex_property_t(), _graph, boost::target(*it, _graph)),
                    edge_info};
            }
        }
        return {nullptr, {}};
    }

    UHD_THROW_INVALID_CODE_PATH();
}

