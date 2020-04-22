#include "baldr/graphreader.h"

#include "midgard/encoded.h"
#include "midgard/logging.h"

#include "baldr/connectivity_map.h"

using namespace valhalla::midgard;

namespace valhalla {
namespace baldr {

// Convenience method to get an opposing directed edge graph Id.
GraphId GraphReader::GetOpposingEdgeId(const GraphId& edgeid, const GraphTile*& tile) {
  // If you cant get the tile you get an invalid id
  tile = GetGraphTile(edgeid);
  if (!tile) {
    return {};
  };
  // For now return an invalid Id if this is a transit edge
  const auto* directededge = tile->directededge(edgeid);
  if (directededge->IsTransitLine()) {
    return {};
  };

  // If edge leaves the tile get the end node's tile
  GraphId id = directededge->endnode();
  if (!GetGraphTile(id, tile)) {
    return {};
  };

  // Get the opposing edge
  id.set_id(tile->node(id)->edge_index() + directededge->opp_index());
  return id;
}

// Convenience method to determine if 2 directed edges are connected.
bool GraphReader::AreEdgesConnected(const GraphId& edge1, const GraphId& edge2) {
  // Check if there is a transition edge between n1 and n2
  auto is_transition = [this](const GraphId& n1, const GraphId& n2) {
    if (n1.level() == n2.level()) {
      return false;
    } else {
      const GraphTile* tile = GetGraphTile(n1);
      const NodeInfo* ni = tile->node(n1);
      if (ni->transition_count() == 0)
        return false;
      const NodeTransition* trans = tile->transition(ni->transition_index());
      for (uint32_t i = 0; i < ni->transition_count(); ++i, ++trans) {
        if (trans->endnode() == n2) {
          return true;
        }
      }
    }
    return false;
  };

  // Get both directed edges
  const GraphTile* t1 = GetGraphTile(edge1);
  const DirectedEdge* de1 = t1->directededge(edge1);
  const GraphTile* t2 = (edge2.Tile_Base() == edge1.Tile_Base()) ? t1 : GetGraphTile(edge2);
  const DirectedEdge* de2 = t2->directededge(edge2);
  if (de1->endnode() == de2->endnode() || is_transition(de1->endnode(), de2->endnode())) {
    return true;
  }

  // Get opposing edge to de1
  const DirectedEdge* de1_opp = GetOpposingEdge(edge1, t1);
  if (de1_opp &&
      (de1_opp->endnode() == de2->endnode() || is_transition(de1_opp->endnode(), de2->endnode()))) {
    return true;
  }

  // Get opposing edge to de2 and compare to both edge1 endnodes
  const DirectedEdge* de2_opp = GetOpposingEdge(edge2, t2);
  if (de2_opp && (de2_opp->endnode() == de1->endnode() || de2_opp->endnode() == de1_opp->endnode() ||
                  is_transition(de2_opp->endnode(), de1->endnode()) ||
                  is_transition(de2_opp->endnode(), de1_opp->endnode()))) {
    return true;
  }
  return false;
}

// Convenience method to determine if 2 directed edges are connected from
// end node of edge1 to the start node of edge2.
bool GraphReader::AreEdgesConnectedForward(const GraphId& edge1,
                                           const GraphId& edge2,
                                           const GraphTile*& tile) {
  // Get end node of edge1
  GraphId endnode = edge_endnode(edge1, tile);
  if (endnode.Tile_Base() != edge1.Tile_Base()) {
    tile = GetGraphTile(endnode);
    if (tile == nullptr) {
      return false;
    }
  }

  // If edge2 is on a different tile level transition to the node on that level
  if (edge2.level() != endnode.level()) {
    for (const auto& trans : tile->GetNodeTransitions(endnode)) {
      if (trans.endnode().level() == edge2.level()) {
        endnode = trans.endnode();
        tile = GetGraphTile(endnode);
        if (tile == nullptr) {
          return false;
        }
        break;
      }
    }
  }

  // Check if edge2's Id is an outgoing directed edge of the node
  const NodeInfo* node = tile->node(endnode);
  return (node->edge_index() <= edge2.id() && edge2.id() < (node->edge_index() + node->edge_count()));
}

// Get the shortcut edge that includes this edge.
GraphId GraphReader::GetShortcut(const GraphId& id) {
  // Lambda to get continuing edge at a node. Skips the specified edge Id
  // transition edges, shortcut edges, and transit connections. Returns
  // nullptr if more than one edge remains or no continuing edge is found.
  auto continuing_edge = [](const GraphTile* tile, const GraphId& edgeid, const NodeInfo* nodeinfo) {
    uint32_t idx = nodeinfo->edge_index();
    const DirectedEdge* continuing_edge = static_cast<const DirectedEdge*>(nullptr);
    const DirectedEdge* directededge = tile->directededge(idx);
    for (uint32_t i = 0; i < nodeinfo->edge_count(); i++, directededge++, idx++) {
      if (idx == edgeid.id() || directededge->is_shortcut() ||
          directededge->use() == Use::kTransitConnection ||
          directededge->use() == Use::kEgressConnection ||
          directededge->use() == Use::kPlatformConnection) {
        continue;
      }
      if (continuing_edge != nullptr) {
        return static_cast<const DirectedEdge*>(nullptr);
      }
      continuing_edge = directededge;
    }
    return continuing_edge;
  };

  // No shortcuts on the local level or transit level.
  if (id.level() >= TileHierarchy::levels().rbegin()->second.level) {
    return {};
  }

  // If this edge is a shortcut return this edge Id
  const GraphTile* tile = GetGraphTile(id);
  const DirectedEdge* directededge = tile->directededge(id);
  if (directededge->is_shortcut()) {
    return id;
  }

  // Walk backwards along the opposing directed edge until a shortcut
  // beginning is found or to get the continuing edge until a node that starts
  // the shortcut is found or there are 2 or more other regular edges at the
  // node.
  GraphId edgeid = id;
  const NodeInfo* node = nullptr;
  const DirectedEdge* cont_de = nullptr;
  while (true) {
    // Get the continuing directed edge. Initial case is to use the opposing
    // directed edge.
    cont_de = (node == nullptr) ? GetOpposingEdge(id) : continuing_edge(tile, edgeid, node);
    if (cont_de == nullptr) {
      return {};
    }

    // Get the end node and end node tile
    GraphId endnode = cont_de->endnode();
    if (cont_de->leaves_tile()) {
      tile = GetGraphTile(endnode.Tile_Base());
    }
    node = tile->node(endnode);

    // Get the opposing edge Id and its directed edge
    uint32_t idx = node->edge_index() + cont_de->opp_index();
    edgeid = {endnode.tileid(), endnode.level(), idx};
    directededge = tile->directededge(edgeid);
    if (directededge->superseded()) {
      // Get the shortcut edge Id that supersedes this edge
      uint32_t idx = node->edge_index() + (directededge->superseded() - 1);
      return GraphId(endnode.tileid(), endnode.level(), idx);
    }
  }
  return {};
}

// Unpack edges for a given shortcut edge
std::vector<GraphId> GraphReader::RecoverShortcut(const GraphId& shortcut_id) {
  // grab the shortcut edge
  const GraphTile* tile = GetGraphTile(shortcut_id);
  const DirectedEdge* shortcut = tile->directededge(shortcut_id);

  // bail if this isnt a shortcut
  if (!shortcut->is_shortcut()) {
    return {shortcut_id};
  }

  // loop over the edges leaving its begin node and find the superseded edge
  GraphId begin_node = edge_startnode(shortcut_id);
  if (!begin_node)
    return {shortcut_id};

  // loop over the edges leaving its begin node and find the superseded edge
  std::vector<GraphId> edges;
  for (const DirectedEdge& de : tile->GetDirectedEdges(begin_node.id())) {
    if (shortcut->shortcut() & de.superseded()) {
      edges.push_back(tile->header()->graphid());
      edges.back().set_id(&de - tile->directededge(0));
      break;
    }
  }

  // bail if we couldnt find it
  if (edges.empty()) {
    LOG_ERROR("Unable to recover shortcut for edgeid " + std::to_string(shortcut_id) +
              " | no superseded edge");
    return {shortcut_id};
  }

  // seed the edge walking with the first edge
  const DirectedEdge* current_edge = tile->directededge(edges.back());
  uint32_t accumulated_length = current_edge->length();

  // walk edges until we find the same ending node as the shortcut
  while (current_edge->endnode() != shortcut->endnode()) {
    // get the node at the end of the last edge we added
    const NodeInfo* node = GetEndNode(current_edge, tile);
    if (!node)
      return {shortcut_id};
    auto node_index = node - tile->node(0);

    // check the edges leaving this node to see if we can find the one that is part of the shortcut
    current_edge = nullptr;
    for (const DirectedEdge& edge : tile->GetDirectedEdges(node_index)) {
      // are they the same enough that its part of the shortcut
      // NOTE: this fails in about .05% of cases where there are two candidates and its not clear
      // which edge is the right one. looking at shortcut builder its not obvious how this is possible
      // as it seems to terminate a shortcut if more than one edge pair can be contracted...
      // NOTE: because we change the speed of the edge in graph enhancer we cant use speed as a
      // reliable determining factor
      if (begin_node != edge.endnode() && !edge.is_shortcut() &&
          (edge.forwardaccess() & kAutoAccess) && edge.sign() == shortcut->sign() &&
          edge.use() == shortcut->use() && edge.classification() == shortcut->classification() &&
          edge.roundabout() == shortcut->roundabout() && edge.link() == shortcut->link() &&
          edge.toll() == shortcut->toll() && edge.destonly() == shortcut->destonly() &&
          edge.unpaved() == shortcut->unpaved() && edge.surface() == shortcut->surface()/* &&
          edge.speed() == shortcut->speed()*/) {
        // we are going to keep this edge
        edges.emplace_back(tile->header()->graphid());
        edges.back().set_id(&edge - tile->directededge(0));
        // and keep expanding from the end of it
        current_edge = &edge;
        begin_node = tile->header()->graphid();
        begin_node.set_id(node_index);
        accumulated_length += edge.length();
        break;
      }
    }

    // if we didnt add an edge or we went over the length we failed
    if (current_edge == nullptr || accumulated_length > shortcut->length()) {
      LOG_ERROR("Unable to recover shortcut for edgeid " + std::to_string(shortcut_id) +
                " | accumulated_length: " + std::to_string(accumulated_length) +
                " | shortcut_length: " + std::to_string(shortcut->length()));
      return {shortcut_id};
    }
  }

  // we somehow got to the end via a shorter path
  if (accumulated_length < shortcut->length()) {
    LOG_ERROR("Unable to recover shortcut for edgeid (accumulated_length < shortcut->length()) " +
              std::to_string(shortcut_id) +
              " | accumulated_length: " + std::to_string(accumulated_length) +
              " | shortcut_length: " + std::to_string(shortcut->length()));
    return {shortcut_id};
  }

  // these edges make up this shortcut
  return edges;
}

// Convenience method to get the relative edge density (from the
// begin node of an edge).
uint32_t GraphReader::GetEdgeDensity(const GraphId& edgeid) {
  // Get the end node of the opposing directed edge
  const DirectedEdge* opp_edge = GetOpposingEdge(edgeid);
  if (opp_edge) {
    GraphId id = opp_edge->endnode();
    const GraphTile* tile = GetGraphTile(id);
    return (tile != nullptr) ? tile->node(id)->density() : 0;
  } else {
    return 0;
  }
}

// Get the end nodes of a directed edge.
std::pair<GraphId, GraphId> GraphReader::GetDirectedEdgeNodes(const GraphTile* tile,
                                                              const DirectedEdge* edge) {
  GraphId end_node = edge->endnode();
  GraphId start_node;
  const GraphTile* t2 = (edge->leaves_tile()) ? GetGraphTile(end_node) : tile;
  if (t2 != nullptr) {
    auto edge_idx = t2->node(end_node)->edge_index() + edge->opp_index();
    start_node = t2->directededge(edge_idx)->endnode();
  }
  return std::make_pair(start_node, end_node);
}

std::string GraphReader::encoded_edge_shape(const valhalla::baldr::GraphId& edgeid) {
  const baldr::GraphTile* t_debug = GetGraphTile(edgeid);
  if (t_debug == nullptr) {
    return {};
  }

  const baldr::DirectedEdge* directedEdge = t_debug->directededge(edgeid);
  auto shape = t_debug->edgeinfo(directedEdge->edgeinfo_offset()).shape();
  if (!directedEdge->forward()) {
    std::reverse(shape.begin(), shape.end());
  }
  return midgard::encode(shape);
}

AABB2<PointLL> GraphReader::GetMinimumBoundingBox(const AABB2<PointLL>& bb) {
  // Iterate through all the tiles that intersect this bounding box
  const auto& ids = TileHierarchy::GetGraphIds(bb);
  AABB2<PointLL> min_bb{PointLL{}, PointLL{}};
  for (const auto& tile_id : ids) {
    // Don't take too much ram
    if (OverCommitted())
      Trim();

    // Look at every node in the tile
    const auto* tile = GetGraphTile(tile_id);
    for (uint32_t i = 0; tile && i < tile->header()->nodecount(); i++) {

      // If the node is within the input bounding box
      const auto* node = tile->node(i);
      auto node_ll = node->latlng(tile->header()->base_ll());
      if (bb.Contains(node_ll)) {

        // If we havent done anything with our bbox yet initialize it
        if (!min_bb.minpt().IsValid())
          min_bb = AABB2<PointLL>(node_ll, node_ll);

        // Look at the shape of each edge leaving the node
        const auto* diredge = tile->directededge(node->edge_index());
        for (uint32_t i = 0; i < node->edge_count(); i++, diredge++) {
          auto shape = tile->edgeinfo(diredge->edgeinfo_offset()).lazy_shape();
          while (!shape.empty()) {
            min_bb.Expand(shape.pop());
          }
        }
      }
    }
  }

  // give back the expanded box
  return min_bb;
}

} // namespace baldr
} // namespace valhalla
