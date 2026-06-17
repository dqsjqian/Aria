// aria_client.js — vanilla JS SDK for the Aria HTTP adapter.
//
// Usage:
//   import { AriaClient } from './aria_client.js';
//   const client = new AriaClient('http://localhost:9090');
//   await client.connect();
//   client.subscribe('search_keyword', (val) => console.log('keyword:', val));
//   client.setText('search_keyword', 'hello world');
//   client.click('search_button');
//   client.command('searchVm', 'search', { matchTitle: true });
//
// Wire protocol: see modules/adapters/http/include/aria/adapters/http/wire_protocol.hpp
//
// Browsers required: any modern browser with EventSource and fetch().
// (IE not supported — we don't apologise for this.)

export class AriaClient {
    /**
     * @param {string} baseUrl  e.g. "http://localhost:9090"
     * @param {object} [opts]
     * @param {string} [opts.apiPrefix="/aria"]  Match server config.api_prefix
     */
    constructor(baseUrl, opts = {}) {
        this.baseUrl = baseUrl.replace(/\/$/, '');
        this.apiPrefix = opts.apiPrefix || '/aria';
        // (viewId, field) -> Set<callback>
        this._stateSubs = new Map();
        // viewId -> Set<callback>
        this._eventSubs = new Map();
        // Latest known shadow state, keyed by viewId.
        this._state = new Map();
        // EventSource instance.
        this._es = null;
        // Connection lifecycle callbacks.
        this._onOpen = [];
        this._onError = [];
        this._connected = false;
    }

    // ── Connection ──────────────────────────────────────────────────────

    /**
     * Open the SSE stream. Resolves when the server's "hello" frame
     * arrives, or rejects on error.
     */
    connect() {
        return new Promise((resolve, reject) => {
            const url = this.baseUrl + this.apiPrefix + '/stream';
            this._es = new EventSource(url);
            this._es.onmessage = (msg) => {
                try {
                    const env = JSON.parse(msg.data);
                    this._dispatch(env);
                    if (env.type === 'hello' && !this._connected) {
                        this._connected = true;
                        for (const cb of this._onOpen) cb();
                        resolve();
                    }
                } catch (e) {
                    console.error('[aria] bad SSE frame:', msg.data, e);
                }
            };
            this._es.onerror = (e) => {
                for (const cb of this._onError) cb(e);
                if (!this._connected) reject(e);
            };
        });
    }

    /** Close the SSE stream. */
    close() {
        if (this._es) { this._es.close(); this._es = null; }
        this._connected = false;
    }

    /** Register a callback fired when (re)connected. */
    onOpen(cb) { this._onOpen.push(cb); }
    /** Register a callback fired on connection error. */
    onError(cb) { this._onError.push(cb); }

    /** True iff `connect()` resolved and the SSE stream is alive. */
    isConnected() { return this._connected; }

    // ── Subscriptions (server → client) ─────────────────────────────────

    /**
     * Subscribe to state changes for a view's primary value field.
     * @param {string} viewId
     * @param {(value:any) => void} cb
     * @returns {() => void}  unsubscribe function
     */
    subscribe(viewId, cb) {
        return this._addSub(this._stateSubs, viewId, cb);
    }

    /**
     * Subscribe to events (clicks) for a view.
     * @param {string} viewId
     * @param {() => void} cb
     * @returns {() => void}  unsubscribe function
     */
    onEvent(viewId, cb) {
        return this._addSub(this._eventSubs, viewId, cb);
    }

    /** Latest known value for a view (server-side shadow state mirror). */
    getState(viewId) {
        return this._state.get(viewId);
    }

    // ── Outbound (client → server) ──────────────────────────────────────

    setText(viewId, value)   { return this._postState(viewId, 'text',   value); }
    setBool(viewId, value)   { return this._postState(viewId, 'bool',   value); }
    setInt(viewId, value)    { return this._postState(viewId, 'int',    value); }
    setInt64(viewId, value)  { return this._postState(viewId, 'int64',  value); }
    setUInt64(viewId, value) { return this._postState(viewId, 'uint64', value); }
    setFloat(viewId, value)  { return this._postState(viewId, 'float',  value); }
    setDouble(viewId, value) { return this._postState(viewId, 'double', value); }

    /** Fire a click event on a view. */
    click(viewId) {
        return fetch(this.baseUrl + this.apiPrefix + '/click', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId }),
        }).then((r) => r.json());
    }

    /** Invoke a custom command. */
    command(viewId, commandName, args = {}) {
        return fetch(this.baseUrl + this.apiPrefix + '/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId, command: commandName, args }),
        }).then((r) => r.json());
    }

    /** Enumerate all registered views (id + kind). */
    listViews() {
        return fetch(this.baseUrl + this.apiPrefix + '/views')
            .then((r) => r.json())
            .then((d) => d.views || []);
    }

    /** Health probe. */
    health() {
        return fetch(this.baseUrl + this.apiPrefix + '/health')
            .then((r) => r.json());
    }

    // ── Internals ───────────────────────────────────────────────────────

    _postState(viewId, field, value) {
        return fetch(this.baseUrl + this.apiPrefix + '/state', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId, field, value }),
        }).then((r) => r.json());
    }

    _addSub(map, viewId, cb) {
        if (!map.has(viewId)) map.set(viewId, new Set());
        map.get(viewId).add(cb);
        return () => {
            const s = map.get(viewId);
            if (s) s.delete(cb);
        };
    }

    _dispatch(env) {
        switch (env.type) {
            case 'state': {
                this._state.set(env.view, env.value);
                const subs = this._stateSubs.get(env.view);
                if (subs) for (const cb of subs) cb(env.value);
                break;
            }
            case 'event': {
                const subs = this._eventSubs.get(env.view);
                if (subs) for (const cb of subs) cb();
                break;
            }
            case 'visibility':
            case 'enabled': {
                // Mirror as state for convenience; consumers can
                // subscribe via subscribe(viewId+':'+type, ...) if they
                // really need to differentiate.
                const key = env.view + '.' + env.type;
                this._state.set(key, env.value);
                const subs = this._stateSubs.get(key);
                if (subs) for (const cb of subs) cb(env.value);
                break;
            }
            case 'list':
            case 'hello':
            case 'ping':
            case 'error':
                /* fall through; future extension */
                break;
            default:
                console.warn('[aria] unknown envelope type:', env.type);
        }
    }
}

export default AriaClient;
