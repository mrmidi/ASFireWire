//
// Topology.hpp
// In-memory IEEE-1394 bus topology model and builder
//
// Responsibilities:
//   • Accumulate SelfID::AlphaRecord entries for one bus reset generation
//   • Build parent/child edges, find root, basic consistency checks
//   • Provide read-only queries for higher layers (discovery/AVC/stream mgr)
//
// Threading:
//   • Not thread-safe. Use on your driver’s default queue / IRQ thread gate.
//

#pragma once

#include <functional>
#include <stdint.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "SelfIDDecode.hpp" // uses SelfID::AlphaRecord + LinkSpeed/PortCode
#include "TopologyTypes.hpp"

class ConfigROMProperties; // fwd (future: per-node 1212 info)

class Topology {
public:
  struct Node {
    NodeId nodeId{}; // index assigned during Finalize()
    PhyId phy{};     // from self-ID
    bool isRoot = false;
    bool linkActive = false;
    uint8_t gapCount = 0;
    SelfID::LinkSpeed speed = SelfID::LinkSpeed::S100;
    bool contender = false;
    uint8_t powerClass = 0;
    bool initiated = false;

    // Port state for up to 16 ports on the PHY.
    PortState ports[16] = {PortState::NotPresent};

    // Derived connectivity (indices into nodes[]).
    std::vector<NodeId> parents; // typically 0 or 1
    std::vector<NodeId> children;

    // Optional: pointer to parsed config ROM properties (when fetched later).
    const ConfigROMProperties *rom = nullptr;
  };

  struct BuildInfo {
    uint32_t generation = 0; // OHCI self-ID generation for this snapshot
    PhyId rootPhy{};         // discovered root PHY (if known)
    bool integrityOk = true; // inverted quadlets etc.
    std::vector<std::string> warnings;
  };

public:
  Topology() = default;

  // Start a new cycle accumulation.
  void BeginCycle(uint32_t generation);

  // Feed one decoded SelfID Alpha record.
  void AddOrUpdateNode(const SelfID::AlphaRecord &rec);

  // Finish building: assign node IDs, derive edges/roles, root, and sanity
  // checks.
  void Finalize();

  // Clear everything.
  void Clear();

  // Queries (valid after Finalize()).
  const BuildInfo &Info() const { return _info; }
  size_t NodeCount() const { return _nodes.size(); }
  const Node *Root() const;                  // nullptr if unknown
  const Node *FindByPhy(PhyId phy) const;    // nullptr if not present
  const Node *FindByNodeId(NodeId id) const; // nullptr if OOB

  // Iterate read-only over nodes.
  void ForEachNode(const std::function<void(const Node &)> &fn) const;

  // Basic topology validation helpers.
  bool HasCycles() const;          // unexpected loops (should be a tree)
  uint8_t MaxHopsFromRoot() const; // bus depth metric
  bool IsConsistent() const; // parent/child reciprocity, single root, etc.

  // Optional: attach config-ROM data to a known node (when AR parsing added).
  bool AttachROM(PhyId phy, const ConfigROMProperties *props);

  // Log the current topology in a readable, concise format.
  void Log() const;

private:
  // Helpers (decl only; implemented in .cpp)
  void buildEdgesFromPorts();
  void deriveRoot();
  void assignNodeIdsStableOrder(); // deterministic NodeId assignment

private:
  BuildInfo _info{};
  std::vector<Node> _nodes;                      // stable after Finalize()
  std::unordered_map<uint8_t, size_t> _phyIndex; // phyId → _nodes index
};
