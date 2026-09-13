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

const MAX_SAFE_INTEGER = BigInt(Number.MAX_SAFE_INTEGER);
const INT64_MIN = -(1n << 63n);
const INT64_MAX = (1n << 63n) - 1n;
const UINT64_MAX = (1n << 64n) - 1n;

// Never convert an unsafe Number to BigInt: precision was already lost
// before this SDK received it. Decimal strings carry the exact wire value.
function integer64(field, value) {
    if (typeof value === 'number') {
        if (!Number.isSafeInteger(value)) {
            throw new RangeError(`${field} requires a safe integer Number, bigint or decimal string`);
        }
    } else if (typeof value === 'string') {
        const decimal = field === 'uint64' ? /^\d+$/ : /^-?\d+$/;
        if (!decimal.test(value) || value.trim() !== value) {
            throw new TypeError(`${field} requires a decimal integer string`);
        }
    } else if (typeof value !== 'bigint') {
        throw new TypeError(`${field} requires a safe integer Number, bigint or decimal string`);
    }
    const integer = BigInt(value);
    const min = field === 'uint64' ? 0n : INT64_MIN;
    const max = field === 'uint64' ? UINT64_MAX : INT64_MAX;
    if (integer < min || integer > max) throw new RangeError(`${field} value is out of range`);
    return integer;
}

function decodeValue(field, value) {
    // Registered but unset views publish null, including after replacement.
    // Preserve it so a snapshot can clear a previously cached integer.
    if (value === null) return null;
    if (field !== 'int64' && field !== 'uint64') return value;
    const integer = integer64(field, value);
    return integer >= -MAX_SAFE_INTEGER && integer <= MAX_SAFE_INTEGER
        ? Number(integer) : integer;
}

export class AriaClient {
    /**
     * @param {string} baseUrl  e.g. "http://localhost:9090"
     * @param {object} [opts]
     * @param {string} [opts.apiPrefix="/aria"]  Match server config.api_prefix
     */
    constructor(baseUrl, opts = {}) {
        this.baseUrl = baseUrl.replace(/\/$/, '');
        this.apiPrefix = opts.apiPrefix || '/aria';
        // Separate channels keep IDs such as "item.enabled" unambiguous.
        this._stateChannels = new Map(['value', 'visibility', 'enabled'].map(field =>
            [field, { values: new Map(), subscribers: new Map() }]));
        // viewId -> Set<callback>
        this._eventSubs = new Map();
        // EventSource instance.
        this._es = null;
        // Connection lifecycle callbacks.
        this._onOpen = [];
        this._onError = [];
        this._connected = false;
        this._connectAttempt = null;
    }

    // ── Connection ──────────────────────────────────────────────────────

    /**
     * Open the SSE stream. Resolves when the server's "hello" frame
     * arrives, or rejects on error/close. Concurrent calls share one
     * promise. After a transient error, EventSource reconnects itself;
     * connect() can wait for its next hello without opening another stream.
     */
    connect() {
        if (this._connected) return Promise.resolve();
        if (this._connectAttempt) return this._connectAttempt.promise;

        const attempt = {};
        attempt.promise = new Promise((resolve, reject) => {
            attempt.resolve = resolve;
            attempt.reject = reject;
        });
        this._connectAttempt = attempt;
        if (this._es) return attempt.promise;

        let es;
        try {
            es = new EventSource(this.baseUrl + this.apiPrefix + '/stream');
        } catch (error) {
            this._settleConnect(error);
            return attempt.promise;
        }
        this._es = es;
        es.onmessage = (msg) => {
            if (this._es !== es) return; // a closed/replaced stream's queued callback
            try {
                const env = JSON.parse(msg.data);
                if (env.type === 'hello') {
                    const opened = !this._connected;
                    this._connected = true;
                    this._settleConnect(); // user callbacks cannot prevent settlement
                    if (opened) this._notify(this._onOpen);
                } else {
                    this._dispatch(env);
                }
            } catch (error) {
                console.error('[aria] bad SSE frame:', msg.data, error);
            }
        };
        es.onerror = (event) => {
            if (this._es !== es) return;
            this._connected = false;
            if (es.readyState === EventSource.CLOSED) {
                this._es = null; // terminal failures require a fresh stream
                es.close();
            }
            this._settleConnect(event instanceof Error
                ? event : new Error('Aria SSE connection failed'));
            this._notify(this._onError, event);
        };
        return attempt.promise;
    }

    /** Close the SSE stream. */
    close() {
        const es = this._es;
        this._es = null;
        this._connected = false;
        this._settleConnect(new Error('Aria SSE connection closed'));
        if (es) es.close();
    }

    /** Register a callback fired when (re)connected. */
    onOpen(cb) { this._onOpen.push(cb); }
    /** Register a callback fired on connection error. */
    onError(cb) { this._onError.push(cb); }

    /** True after hello, until the stream reports an error or is closed. */
    isConnected() { return this._connected; }

    // ── Subscriptions (server → client) ─────────────────────────────────

    /**
     * Subscribe to state changes for a view's primary value field.
     * @param {string} viewId
     * @param {(value:any) => void} cb
     * @param {'value'|'visibility'|'enabled'} [field='value']
     * @returns {() => void}  unsubscribe function
     */
    subscribe(viewId, cb, field = 'value') {
        return this._addSub(this._stateChannel(field).subscribers, viewId, cb);
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

    /** Latest known value. Int64/uint64 values outside Number's safe range are BigInt. */
    getState(viewId, field = 'value') {
        return this._stateChannel(field).values.get(viewId);
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
        return this._fetchJson('/click', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId }),
        });
    }

    /** Invoke a custom command. */
    command(viewId, commandName, args = {}) {
        return this._fetchJson('/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId, command: commandName, args }),
        });
    }

    /** Enumerate all registered views (id + kind). */
    listViews() {
        return this._fetchJson('/views')
            .then((d) => d.views || []);
    }

    /** Health probe. */
    health() {
        return this._fetchJson('/health');
    }

    // ── Internals ───────────────────────────────────────────────────────

    _settleConnect(error) {
        const attempt = this._connectAttempt;
        this._connectAttempt = null;
        if (!attempt) return;
        if (error) attempt.reject(error);
        else attempt.resolve();
    }

    _notify(callbacks, ...args) {
        for (const cb of Array.from(callbacks)) {
            try { cb(...args); }
            catch (error) { console.error('[aria] callback failed:', error); }
        }
    }

    async _fetchJson(path, options) {
        const response = await fetch(this.baseUrl + this.apiPrefix + path, options);
        if (!response.ok) {
            let detail = '';
            try { detail = (await response.text()).trim(); } catch { /* status still available */ }
            throw new Error(`Aria HTTP ${response.status} ${path}${detail ? ': ' + detail : ''}`);
        }
        return response.json();
    }

    async _postState(viewId, field, value) {
        if (field === 'int64' || field === 'uint64') {
            const integer = integer64(field, value);
            value = integer >= -MAX_SAFE_INTEGER && integer <= MAX_SAFE_INTEGER
                ? Number(integer) : integer.toString();
        }
        return this._fetchJson('/state', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ view: viewId, field, value }),
        });
    }

    _addSub(map, viewId, cb) {
        if (typeof cb !== 'function') throw new TypeError('subscriber must be a function');
        if (!map.has(viewId)) map.set(viewId, new Set());
        const subscribers = map.get(viewId);
        let active = true;
        const invoke = (...args) => { if (active) cb(...args); };
        subscribers.add(invoke);
        return () => {
            if (!active) return;
            active = false;
            subscribers.delete(invoke);
            if (!subscribers.size && map.get(viewId) === subscribers) map.delete(viewId);
        };
    }

    _stateChannel(field) {
        const channel = this._stateChannels.get(field);
        if (!channel) throw new RangeError('state field must be value, visibility or enabled');
        return channel;
    }

    _dispatchState(viewId, field, value) {
        const channel = this._stateChannel(field);
        channel.values.set(viewId, value);
        const subscribers = channel.subscribers.get(viewId);
        if (subscribers) this._notify(subscribers, value);
    }

    _dispatch(env) {
        switch (env.type) {
            case 'state': {
                const value = decodeValue(env.field, env.value);
                this._dispatchState(env.view, 'value', value);
                break;
            }
            case 'event': {
                const subs = this._eventSubs.get(env.view);
                if (subs) this._notify(subs);
                break;
            }
            case 'visibility':
            case 'enabled': {
                this._dispatchState(env.view, env.type, env.value);
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
