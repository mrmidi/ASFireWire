// Topology.cpp
// Minimal implementation of topology builder (skeleton)

#include "Topology.hpp"
#include "BridgeLog.hpp"
#include "LogHelper.hpp"
#include <algorithm>
#include <os/log.h>

void Topology::BeginCycle(uint32_t generation) {
  _info = {};
  _info.generation = generation;
  _nodes.clear();
  _phyIndex.clear();
}

void Topology::AddOrUpdateNode(const SelfID::AlphaRecord &rec) {
  uint8_t phy = rec.phyId;
  auto it = _phyIndex.find(phy);
  if (it == _phyIndex.end()) {
    Node n{};
    n.phy = PhyId{phy};
    n.linkActive = rec.linkActive;
    n.gapCount = rec.gapCount;
    n.speed = rec.speed;
    n.contender = rec.contender;
    n.powerClass = rec.powerClass;
    n.initiated = rec.initiated;
    for (int i = 0; i < 16; ++i) {
      // static_cast is safe due to aligned enums between PortState and
      // SelfID::PortCode
      n.ports[i] = static_cast<PortState>(static_cast<uint8_t>(rec.ports[i]));
    }
    _phyIndex.emplace(phy, _nodes.size());
    _nodes.push_back(n);
  } else {
    Node &n = _nodes[it->second];
    n.linkActive = rec.linkActive;
    n.gapCount = rec.gapCount;
    n.speed = rec.speed;
    n.contender = rec.contender;
    n.powerClass = rec.powerClass;
    n.initiated = rec.initiated;
    for (int i = 0; i < 16; ++i) {
      n.ports[i] = static_cast<PortState>(static_cast<uint8_t>(rec.ports[i]));
    }
  }
}

void Topology::buildEdgesFromPorts() {
  // IEEE 1394-2008 Annex P: Construct explicit parent/child edges from port
  // states Each Parent port on node A should match with exactly one Child port
  // on node B

  // Clear existing adjacency lists
  for (auto &n : _nodes) {
    n.parents.clear();
    n.children.clear();
  }

  uint32_t edgesConstructed = 0;
  uint32_t orphanedPorts = 0;

  // For each node with Parent ports, find corresponding Child ports on other
  // nodes
  for (size_t i = 0; i < _nodes.size(); ++i) {
    Node &nodeA = _nodes[i];

    for (int portA = 0; portA < 16; ++portA) {
      if (nodeA.ports[portA] == PortState::Parent) {
        bool foundMatch = false;

        // Search all other nodes for corresponding Child port
        for (size_t j = 0; j < _nodes.size(); ++j) {
          if (i == j)
            continue; // Skip self
          Node &nodeB = _nodes[j];

          // Look for unused Child port (not already connected)
          for (int portB = 0; portB < 16; ++portB) {
            if (nodeB.ports[portB] == PortState::Child) {
              // Verify this Child port isn't already connected
              bool alreadyConnected = false;
              for (NodeId existingParent : nodeB.parents) {
                if (existingParent == nodeA.nodeId) {
                  alreadyConnected = true;
                  break;
                }
              }

              if (!alreadyConnected) {
                // Create bidirectional edge: A→B (A is parent of B)
                nodeA.children.push_back(nodeB.nodeId);
                nodeB.parents.push_back(nodeA.nodeId);
                edgesConstructed++;
                foundMatch = true;
                break;
              }
            }
          }
          if (foundMatch)
            break;
        }

        if (!foundMatch) {
          orphanedPorts++;
          _info.warnings.push_back("Orphaned Parent port on PHY " +
                                   std::to_string(nodeA.phy.value) + " port " +
                                   std::to_string(portA));
        }
      }
    }
  }

  // Verify tree structure: should have exactly N-1 edges for N nodes
  if (_nodes.size() > 0 && edgesConstructed != (_nodes.size() - 1)) {
    _info.warnings.push_back(
        "Edge count " + std::to_string(edgesConstructed) + " != expected " +
        std::to_string(_nodes.size() - 1) + " for tree structure");
  }

  if (orphanedPorts > 0) {
    _info.warnings.push_back("Found " + std::to_string(orphanedPorts) +
                             " orphaned Parent ports");
  }
}

void Topology::deriveRoot() {
  // Prefer the node with zero Parent ports (root should have no parent link).
  auto countParent = [](const Node &n) {
    uint32_t c = 0;
    for (auto ps : n.ports)
      if (ps == PortState::Parent)
        ++c;
    return c;
  };

  for (auto &n : _nodes)
    n.isRoot = false;

  for (const auto &n : _nodes) {
    if (countParent(n) == 0) {
      _info.rootPhy = n.phy;
      // Mark the first such node as root; multiple zeros indicates
      // inconsistency.
      break;
    }
  }
  if (!_info.rootPhy.valid()) {
    // Fallback: pick the first contender if no zero-parent node found.
    for (const auto &n : _nodes) {
      if (n.contender) {
        _info.rootPhy = n.phy;
        break;
      }
    }
  }
  if (_info.rootPhy.valid()) {
    auto it = _phyIndex.find(_info.rootPhy.value);
    if (it != _phyIndex.end())
      _nodes[it->second].isRoot = true;
  }
}

void Topology::assignNodeIdsStableOrder() {
  for (size_t i = 0; i < _nodes.size(); ++i) {
    _nodes[i].nodeId = NodeId{static_cast<uint8_t>(i)};
  }
}

void Topology::Finalize() {
  assignNodeIdsStableOrder();
  buildEdgesFromPorts();
  deriveRoot();

  // Basic integrity summary: parent vs child tallies and expected edges count.
  uint32_t parents = 0, children = 0;
  for (const auto &n : _nodes) {
    for (auto ps : n.ports) {
      if (ps == PortState::Parent)
        ++parents;
      else if (ps == PortState::Child)
        ++children;
    }
  }
  if (parents != children) {
    _info.warnings.push_back("Parent/Child port counts mismatch");
  }
  // In a tree of N nodes, there should be N-1 links; each link contributes one
  // parent and one child across nodes, so parents == children == N-1 ideally.
  if (_nodes.size() > 0) {
    if (parents != (_nodes.size() - 1)) {
      _info.warnings.push_back("Total link count (parents) != N-1");
    }
  }
}

void Topology::Clear() {
  _info = {};
  _nodes.clear();
  _phyIndex.clear();
}

const Topology::Node *Topology::Root() const {
  if (!_info.rootPhy.valid())
    return nullptr;
  auto it = _phyIndex.find(_info.rootPhy.value);
  if (it == _phyIndex.end())
    return nullptr;
  return &_nodes[it->second];
}

const Topology::Node *Topology::FindByPhy(PhyId phy) const {
  auto it = _phyIndex.find(phy.value);
  if (it == _phyIndex.end())
    return nullptr;
  return &_nodes[it->second];
}

const Topology::Node *Topology::FindByNodeId(NodeId id) const {
  if (!id.valid())
    return nullptr;
  if (static_cast<size_t>(id.value) >= _nodes.size())
    return nullptr;
  return &_nodes[id.value];
}

void Topology::ForEachNode(const std::function<void(const Node &)> &fn) const {
  for (const auto &n : _nodes)
    fn(n);
}

bool Topology::HasCycles() const {
  // Without explicit adjacency, use a coarse check: if total links >= N then
  // cycles likely.
  uint32_t parents = 0;
  for (const auto &n : _nodes)
    for (auto ps : n.ports)
      if (ps == PortState::Parent)
        ++parents;
  return (_nodes.size() > 0) && (parents >= _nodes.size());
}

uint8_t Topology::MaxHopsFromRoot() const {
  const Node *root = Root();
  if (!root || _nodes.empty())
    return 0;

  // BFS traversal to find maximum distance from root
  std::vector<bool> visited(_nodes.size(), false);
  std::vector<uint8_t> distance(_nodes.size(), 0);
  std::vector<NodeId> queue;

  // Start BFS from root
  queue.push_back(root->nodeId);
  visited[root->nodeId.value] = true;
  distance[root->nodeId.value] = 0;

  uint8_t maxHops = 0;
  size_t queueStart = 0;

  while (queueStart < queue.size()) {
    NodeId currentId = queue[queueStart++];
    const Node *current = FindByNodeId(currentId);
    if (!current)
      continue;

    uint8_t currentDistance = distance[currentId.value];

    // Visit all children (traverse down the tree)
    for (NodeId childId : current->children) {
      if (childId.valid() && childId.value < _nodes.size() &&
          !visited[childId.value]) {
        visited[childId.value] = true;
        distance[childId.value] = currentDistance + 1;
        maxHops = std::max(maxHops, static_cast<uint8_t>(currentDistance + 1));
        queue.push_back(childId);
      }
    }
  }

  return maxHops;
}

bool Topology::IsConsistent() const {
  if (_nodes.empty())
    return true;

  // Count port states and validate basic tree structure
  uint32_t parentPorts = 0, childPorts = 0, roots = 0;
  for (const auto &n : _nodes) {
    if (n.isRoot)
      ++roots;
    for (auto ps : n.ports) {
      if (ps == PortState::Parent)
        ++parentPorts;
      else if (ps == PortState::Child)
        ++childPorts;
    }
  }

  // Tree structure validation
  if (roots != 1)
    return false; // Must have exactly one root
  if (parentPorts != childPorts)
    return false; // Reciprocal parent/child ports
  if (parentPorts != (_nodes.size() - 1))
    return false; // N-1 edges for N nodes

  // Verify bidirectional edge reciprocity (IEEE 1394 Annex P requirement)
  for (const auto &node : _nodes) {
    // For each parent relationship, verify the parent lists this node as a
    // child
    for (NodeId parentId : node.parents) {
      const Node *parent = FindByNodeId(parentId);
      if (!parent)
        return false; // Invalid parent reference

      bool foundReciprocal = false;
      for (NodeId childId : parent->children) {
        if (childId == node.nodeId) {
          foundReciprocal = true;
          break;
        }
      }
      if (!foundReciprocal)
        return false; // Missing reciprocal edge
    }

    // For each child relationship, verify the child lists this node as a parent
    for (NodeId childId : node.children) {
      const Node *child = FindByNodeId(childId);
      if (!child)
        return false; // Invalid child reference

      bool foundReciprocal = false;
      for (NodeId parentId : child->parents) {
        if (parentId == node.nodeId) {
          foundReciprocal = true;
          break;
        }
      }
      if (!foundReciprocal)
        return false; // Missing reciprocal edge
    }
  }

  return true;
}

bool Topology::AttachROM(PhyId phy, const ConfigROMProperties *props) {
  auto it = _phyIndex.find(phy.value);
  if (it == _phyIndex.end())
    return false;
  _nodes[it->second].rom = props;
  return true;
}

static inline const char *speedStr(SelfID::LinkSpeed s) {
  switch (s) {
  case SelfID::LinkSpeed::S100:
    return "S100";
  case SelfID::LinkSpeed::S200:
    return "S200";
  case SelfID::LinkSpeed::S400:
    return "S400";
  default:
    return "RES";
  }
}

void Topology::Log() const {
  const BuildInfo &bi = _info;
  size_t nodes = _nodes.size();
  const Topology::Node *root = Root();
  uint8_t hops = MaxHopsFromRoot();
  bool ok = IsConsistent();
  os_log(ASLog(),
         "ASOHCI: === Topology Snapshot === gen=%u nodes=%lu rootPhy=%u "
         "hops=%u consistent=%d warnings=%lu",
         bi.generation, (unsigned long)nodes, root ? root->phy.value : 0xFF,
         hops, ok ? 1 : 0, (unsigned long)bi.warnings.size());
  if (!bi.warnings.empty()) {
    for (const auto &w : bi.warnings)
      os_log(ASLog(), "ASOHCI:  warn: %{public}s", w.c_str());
  }
  for (const auto &n : _nodes) {
    uint32_t pParents = 0, pChildren = 0, present = 0, active = 0;
    char portLine[17];
    portLine[16] = '\0';
    for (int i = 0; i < 16; ++i) {
      char c = '-';
      switch (n.ports[i]) {
      case PortState::NotPresent:
        c = '-';
        break;
      case PortState::NotActive:
        c = '.';
        ++present;
        break;
      case PortState::Parent:
        c = 'P';
        ++present;
        ++active;
        ++pParents;
        break;
      case PortState::Child:
        c = 'C';
        ++present;
        ++active;
        ++pChildren;
        break;
      }
      portLine[i] = c;
    }
    os_log(ASLog(),
           "ASOHCI:  node phy=%u id=%u %s L=%u gap=%u sp=%s cont=%u pwr=%u "
           "init=%u ports[%u/%u]: %s",
           n.phy.value, n.nodeId.valid() ? n.nodeId.value : 0xFF,
           (n.isRoot ? "(root)" : ""), n.linkActive ? 1 : 0, n.gapCount,
           speedStr(n.speed), n.contender ? 1 : 0, n.powerClass,
           n.initiated ? 1 : 0, active, present, portLine);
    // Print adjacency by PHY where possible
    if (!n.parents.empty() || !n.children.empty()) {
      // Build small lists of PHY ids
      char buf[128];
      buf[0] = '\0';
      size_t off = 0;
      if (!n.parents.empty()) {
        off += snprintf(buf + off, sizeof(buf) - off, " parents:");
        for (auto pid : n.parents) {
          const Node *pn = FindByNodeId(pid);
          off += snprintf(buf + off, sizeof(buf) - off, " %u",
                          pn ? pn->phy.value : 0xFF);
        }
      }
      if (!n.children.empty()) {
        off += snprintf(buf + off, sizeof(buf) - off, " children:");
        for (auto cid : n.children) {
          const Node *cn = FindByNodeId(cid);
          off += snprintf(buf + off, sizeof(buf) - off, " %u",
                          cn ? cn->phy.value : 0xFF);
        }
      }
      os_log(ASLog(), "ASOHCI:   adj:%{public}s", buf);
    }
  }
  os_log(ASLog(), "ASOHCI: === End Topology ===");
}
