// Copyright (c) 2026 Nordic Semiconductor ASA
//
// SPDX-License-Identifier: LicenseRef-Nordic-5-Clause

// ─────────────────────────────────────────────────────────────────────────────
// Mesh Configuration Database
//
// Everything the website knows about the mesh network, based on the Bluetooth SIG
// "Mesh Configuration Database Profile 1.0.1" JSON format
//
// Only the parts of the schema the website actually fills in are built here:
//   netKeys[]  a subnet's NetIdx + NetKey
//   appKeys[]  an AppKey, with the subnet it is bound to
//   nodes[]    address, UUID, device key, element count, configured flag, the
//              subnet and AppKey it was provisioned onto, and per element the
//              SIG models it reports with the group they were configured against
// Forms the schema insists on: addresses are 4 hex digits with no 0x prefix,
// keys are 32 hex digits, and UUIDs are dashed 8-4-4-4-12 (the gateway speaks
// plain 32-hex, so uuidDashed/uuidPlain convert at that boundary).
// ─────────────────────────────────────────────────────────────────────────────

const Mesh = (function () {
    const STORE_KEY = "meshCdb";

    // The gateway provisions itself with this fixed UUID, which is how a CDB
    // dump line is recognised as the gateway's own node.
    const GATEWAY_UUID = "DDDD0000-0000-0000-0000-000000000000";

    // Every mesh node instantiates a Configuration Server on its primary
    // element — the spec requires it. Recorded so an element always has the one
    // model the schema asks of it, even on a node whose composition data we
    // never read (the gateway's own), and filtered back out of nodeModels()
    // since it says nothing about what the node can do.
    const CONFIG_SRV = "0000";

    // ── hex, address and UUID helpers ────────────────────────────────────────

    // Schema form of an address: 4 uppercase hex digits, no prefix. Accepts a
    // number, or hex text with or without the 0x the gateway prints ("0x0002",
    // "C001") — never decimal text, since the schema form is itself bare hex.
    function addr4(value) {
        const n = typeof value === "number"
            ? value
            : Number.parseInt(String(value ?? "").trim().replace(/^0[xX]/, ""), 16);
        if (!Number.isFinite(n) || n < 0) return "0000";
        return n.toString(16).toUpperCase().padStart(4, "0");
    }

    function addrNum(value) {
        return Number.parseInt(addr4(value), 16);
    }

    // Display and gtw-command form used throughout the UI.
    function addrText(value) {
        return "0x" + addr4(value).toLowerCase();
    }

    // A 128-bit key in schema form, or "" when the input isn't one.
    function key32(value) {
        const text = String(value ?? "").trim();
        return /^[0-9a-fA-F]{32}$/.test(text) ? text.toUpperCase() : "";
    }

    function uuidDashed(value) {
        const p = key32(value);
        if (p) return [p.slice(0, 8), p.slice(8, 12), p.slice(12, 16), p.slice(16, 20), p.slice(20)].join("-");
        const text = String(value ?? "").trim().toUpperCase();
        return /^[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}$/.test(text) ? text : "";
    }

    function uuidPlain(value) {
        const text = String(value ?? "").trim().replace(/-/g, "");
        return /^[0-9a-fA-F]{32}$/.test(text) ? text.toLowerCase() : "";
    }

    // ── the database ─────────────────────────────────────────────────────────

    // The gateway is the network's only provisioner, with the ranges it hands
    // out: unicast addresses up the range, groups from the 0xC000 block.
    const PROVISIONER = {
        provisionerName: "ESI Cloud Gateway",
        UUID: GATEWAY_UUID,
        allocatedUnicastRange: [{ lowAddress: "0001", highAddress: "7FFF" }],
        allocatedGroupRange: [{ lowAddress: "C000", highAddress: "FEFF" }],
        allocatedSceneRange: [{ firstScene: "0001", lastScene: "FFFF" }],
    };

    function emptyDb() {
        return {
            $schema: "http://json-schema.org/draft-04/schema#",
            id: "https://www.bluetooth.com/specifications/specs/mesh-cdb-1-0-1-schema.json#",
            version: "1.0.1",
            meshName: "ESI Cloud Mesh",
            meshUUID: (window.crypto.randomUUID?.() || "").toUpperCase(),
            timestamp: new Date().toISOString(),
            partial: true,
            provisioners: [PROVISIONER],
            netKeys: [],
            appKeys: [],
            nodes: [],
            groups: [],
            scenes: [],
        };
    }

    let db = emptyDb();

    function save() {
        // The schema's flag for a database that doesn't fully describe the
        // network: true until a Config DB save has filled in the device keys.
        db.partial = db.nodes.some(n => !n.deviceKey);
        db.timestamp = new Date().toISOString();
        try {
            localStorage.setItem(STORE_KEY, JSON.stringify(db));
        } catch (e) { /* storage full or blocked — the in-memory db still works */ }
    }

    try {
        const stored = JSON.parse(localStorage.getItem(STORE_KEY) || "null");
        if (stored && Array.isArray(stored.nodes)) db = Object.assign(emptyDb(), stored);
    } catch (e) { /* corrupt store — start from an empty db */ }

    // Forget the whole network: drop the stored copy and start from an empty db,
    // without writing anything back.
    function reset() {
        db = emptyDb();
        try {
            localStorage.removeItem(STORE_KEY);
        } catch (e) { /* storage blocked — the in-memory db is already empty */ }
    }

    function isEmpty() {
        return db.nodes.length === 0 && db.netKeys.length === 0 && db.appKeys.length === 0;
    }

    // ── subnets and AppKeys ──────────────────────────────────────────────────

    function netKey(index) {
        return db.netKeys.find(k => k.index === Number(index)) || null;
    }

    // Updated in place: after a Key Refresh the same NetIdx carries a new key,
    // and replaceNode() must never rebuild the network with the revoked one.
    function upsertNetKey(index, key) {
        let entry = netKey(index);
        if (!entry) {
            entry = { name: "Subnet " + index, index: Number(index), phase: 0, key: "", minSecurity: "secure" };
            db.netKeys.push(entry);
            db.netKeys.sort((a, b) => a.index - b.index);
        }
        entry.key = key32(key) || entry.key;
        return entry;
    }

    function appKey(index) {
        return db.appKeys.find(k => k.index === Number(index)) || null;
    }

    function upsertAppKey(index, boundNetKey, key) {
        let entry = appKey(index);
        if (!entry) {
            entry = { name: "App Key " + index, index: Number(index), boundNetKey: 0, key: "" };
            db.appKeys.push(entry);
            db.appKeys.sort((a, b) => a.index - b.index);
        }
        entry.boundNetKey = Number(boundNetKey);
        entry.key = key32(key) || entry.key;
        return entry;
    }

    // A Bluetooth Mesh AppKey belongs to exactly one subnet — the schema gives
    // each AppKey a single boundNetKey. Returns the subnet an AppKey index is
    // already bound to when it differs from netIdx, else null. The gateway
    // enforces the same rule authoritatively.
    function appKeyBoundToOtherNet(appIdx, netIdx) {
        const entry = appKey(appIdx);
        if (!entry) return null;
        return entry.boundNetKey === Number(netIdx) ? null : entry.boundNetKey;
    }

    // ── nodes ────────────────────────────────────────────────────────────────

    function node(address) {
        return db.nodes.find(n => n.unicastAddress === addr4(address)) || null;
    }

    function nodeByUuid(uuid) {
        const want = uuidDashed(uuid);
        return want ? db.nodes.find(n => n.UUID === want) || null : null;
    }

    // Every node the website offers controls for. The gateway is a node too,
    // but it is the provisioner, not something the network panel drives.
    function provisionedNodes() {
        return db.nodes.filter(n => n.UUID !== GATEWAY_UUID);
    }

    // Add or update a node. Only the fields present in `info` are written, so a
    // source that doesn't know a value (the `mesh cdb show` dump carries no
    // net/app index; a Provisioning message carries no device key) can never
    // clobber what another source already established.
    function upsertNode(info) {
        const uuid = uuidDashed(info.uuid);
        const address = addr4(info.address);
        if (addrNum(address) === 0) return null;

        // Match on UUID first: a replaced node keeps its UUID but may land on a
        // different unicast address.
        let n = (uuid && nodeByUuid(uuid)) || node(address);
        if (!n) {
            n = {
                UUID: uuid,
                name: uuid === GATEWAY_UUID ? "Gateway" : "Node " + addrText(address),
                unicastAddress: address,
                security: "secure",
                deviceKey: "",
                configComplete: false,
                excluded: false,
                netKeys: [{ index: 0, updated: false }],
                appKeys: [{ index: 0, updated: false }],
                elements: [],
            };
            db.nodes.push(n);
        }

        if (uuid) n.UUID = uuid;
        n.unicastAddress = address;
        n.deviceKey = key32(info.deviceKey) || n.deviceKey;
        if (info.configComplete !== undefined) n.configComplete = !!info.configComplete;
        if (info.netIdx !== undefined) n.netKeys = [{ index: Number(info.netIdx), updated: false }];
        if (info.appIdx !== undefined) n.appKeys = [{ index: Number(info.appIdx), updated: false }];
        if (info.elementCount !== undefined) resizeElements(n, info.elementCount);

        db.nodes.sort((a, b) => addrNum(a.unicastAddress) - addrNum(b.unicastAddress));
        return n;
    }

    // Grow or shrink a node's element list, keeping the models already recorded
    // on the elements that survive.
    function resizeElements(n, count) {
        const want = Math.max(1, Number.parseInt(count, 10) || 1);
        while (n.elements.length < want) {
            n.elements.push({
                index: n.elements.length,
                location: "0000",
                models: [{ modelId: CONFIG_SRV, bind: [], subscribe: [] }],
            });
        }
        n.elements.length = want;
    }
    
    function addNodeKey(list, index, primary) {
        const i = Number(index);
        const at = list.findIndex(e => e.index === i);
        if (at >= 0 && !primary) return list; // already recorded, order unchanged
        const entry = at >= 0 ? list.splice(at, 1)[0] : { index: i, updated: false };
        if (primary) list.unshift(entry);
        else list.push(entry);
        return list;
    }

    function addNodeNetKey(address, index, primary) {
        const n = node(address);
        if (n) addNodeKey(n.netKeys, index, primary);
        return n;
    }

    function addNodeAppKey(address, index, primary) {
        const n = node(address);
        if (n) addNodeKey(n.appKeys, index, primary);
        return n;
    }

    // The subnet this node's commands are sent on
    function nodeNetIdx(address) {
        const n = node(address);
        return n && n.netKeys.length ? n.netKeys[0].index : 0;
    }

    // Does this node hold a subnet? Config AppKey Add names the AppKey's own
    // subnet, so a node that is not on that subnet cannot be bound to the key.
    function nodeHasNetKey(address, index) {
        const n = node(address);
        return !!n && n.netKeys.some(k => k.index === Number(index));
    }

    function nodeAppIdx(address) {
        const n = node(address);
        return n && n.appKeys.length ? n.appKeys[0].index : 0;
    }

    // Drop a subnet from a node. Deleting a NetKey on a node takes every AppKey
    // bound to that NetKey with it, so the mirror drops those too.
    function removeNodeNetKey(address, index) {
        const n = node(address);
        if (!n) return null;
        const i = Number(index);
        n.netKeys = n.netKeys.filter(k => k.index !== i);
        n.appKeys = n.appKeys.filter(k => {
            const bound = appKey(k.index);
            return !bound || bound.boundNetKey !== i;
        });
        return n;
    }

    // Drop one AppKey from a node, leaving its subnets alone.
    function removeNodeAppKey(address, index) {
        const n = node(address);
        if (!n) return null;
        n.appKeys = n.appKeys.filter(k => k.index !== Number(index));
        return n;
    }

    // Drop a group address from a node: out of every model's subscription list,
    // and out of its publication when that is what it pointed at. The group
    // itself stays in db.groups: it is the provisioner's, and other nodes may
    // still be on it.
    function removeNodeGroup(address, group) {
        const n = node(address);
        if (!n) return null;
        const addr = addr4(group);
        n.elements.forEach(el => el.models.forEach(m => {
            m.subscribe = (m.subscribe || []).filter(s => addr4(s) !== addr);
            if (m.publish && addr4(m.publish.address) === addr) delete m.publish;
        }));
        return n;
    }

    // The SIG model IDs (as numbers) a node reports.
    function nodeModels(address) {
        const n = node(address);
        if (!n) return [];
        return n.elements
            .flatMap(el => el.models.filter(m => m.modelId !== CONFIG_SRV))
            .map(m => Number.parseInt(m.modelId, 16))
            .filter(id => !Number.isNaN(id));
    }

    // Every group a node is subscribed to, as display strings ("0xc001"), in the
    // order they were configured. Config Model Subscription Add only ever adds,
    // so a node that has been configured more than once holds all of them.
    function nodeGroups(address) {
        const n = node(address);
        if (!n) return [];
        const seen = [];
        n.elements.forEach(el => el.models.forEach(m => (m.subscribe || []).forEach(s => {
            const addr = addr4(s);
            if (addrNum(addr) !== 0 && !seen.includes(addr)) seen.push(addr);
        })));
        return seen.map(addrText);
    }

    // The group a node publishes to, as a display string, or "" when there is
    // none. Publication is single-valued, so the latest configuration replaced
    // whatever it pointed at before — unlike the subscriptions above.
    function nodeGroup(address) {
        const n = node(address);
        if (!n) return "";
        const pub = n.elements
            .flatMap(el => el.models)
            .map(m => m.publish && m.publish.address)
            .find(a => a && addrNum(a) !== 0);
        return pub ? addrText(pub) : "";
    }

    // Record a node's composition data: the SIG models the gateway read back
    // from it, bound to the AppKey and pointed at the control group it was
    // configured with. The firmware reports one model set per node
    // (de-duplicated across its elements in read_node_models()), so it lands on
    // the primary element.
    function setNodeComposition(address, modelIds, appIdx, group) {
        const n = node(address);
        if (!n) return;
        if (n.elements.length === 0) resizeElements(n, 1);

        const addr = group ? addr4(group) : "";
        const subscribe = nodeGroups(address).map(addr4);

        if (addr && !subscribe.includes(addr)) subscribe.push(addr);

        n.elements[0].models = [{ modelId: CONFIG_SRV, bind: [], subscribe: [] }].concat(
            modelIds.filter(id => addr4(id) !== CONFIG_SRV).map(id => {
                const model = {
                    modelId: addr4(id),
                    bind: [Number(appIdx) || 0],
                    subscribe: subscribe.slice(),
                };

                if (addr) model.publish = { address: addr, index: Number(appIdx) || 0 };
                return model;
            }));

        if (addr && !db.groups.some(g => g.address === addr)) {
            db.groups.push({ name: "Group " + addrText(addr), address: addr, parentAddress: "0000" });
        }
    }

    // Lowest unicast address not covered by any node's element range — where a
    // rebuilt gateway should land.
    function nextFreeAddress() {
        return db.nodes.reduce(
            (max, n) => Math.max(max, addrNum(n.unicastAddress) + Math.max(1, n.elements.length) - 1), 0) + 1;
    }

    // Drop everything the gateway no longer has. Called once a `mesh cdb show`
    // dump has been read in full, with the addresses and key indices it listed,
    // so an evicted node doesn't linger — and so a lost dump line, which just
    // leaves the dump unfinished, can never delete anything.
    function pruneTo(nodeAddrs, netIdxs, appIdxs) {
        db.nodes = db.nodes.filter(n => nodeAddrs.includes(n.unicastAddress));
        db.netKeys = db.netKeys.filter(k => netIdxs.includes(k.index));
        db.appKeys = db.appKeys.filter(k => appIdxs.includes(k.index));
        save();
    }

    return {
        get db() { return db; },
        addr4, addrNum, addrText, uuidPlain,
        save, reset, isEmpty, pruneTo,
        netKey, appKey, upsertNetKey, upsertAppKey, appKeyBoundToOtherNet,
        node, nodeByUuid, provisionedNodes, upsertNode,
        addNodeNetKey, addNodeAppKey, removeNodeNetKey, removeNodeAppKey, removeNodeGroup,
        nodeNetIdx, nodeAppIdx, nodeHasNetKey, nodeModels, nodeGroup, nodeGroups,
        setNodeComposition, nextFreeAddress,
    };
})();
