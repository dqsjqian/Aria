import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import test from 'node:test';

// Import the browser ES module without requiring a package.json or a bundler.
const source = await readFile(new URL('./aria_client.js', import.meta.url), 'utf8');
const { AriaClient } = await import('data:text/javascript;base64,' + Buffer.from(source).toString('base64'));

function setup(t) {
    const streams = [];
    const requests = [];
    const diagnostics = [];
    class MockEventSource {
        static CLOSED = 2;
        static failConstruction = false;
        constructor(url) {
            if (MockEventSource.failConstruction) throw new Error('constructor failed');
            this.url = url;
            this.readyState = 0;
            this.closeCount = 0;
            streams.push(this);
        }
        emit(env) {
            this.readyState = 1;
            this.onmessage({ data: JSON.stringify(env) });
        }
        fail(terminal = false) {
            this.readyState = terminal ? 2 : 0;
            this.onerror({ type: 'error' });
        }
        close() { this.readyState = 2; ++this.closeCount; }
    }
    const previous = globalThis.EventSource;
    globalThis.EventSource = MockEventSource;
    t.after(() => {
        if (previous === undefined) delete globalThis.EventSource;
        else globalThis.EventSource = previous;
    });
    t.mock.method(console, 'error', (...args) => diagnostics.push(args));
    t.mock.method(globalThis, 'fetch', async (url, options) => {
        requests.push({ url, options });
        return new Response(JSON.stringify({ ok: true, views: [{ id: 'value', kind: 'int64' }] }));
    });
    const client = new AriaClient('http://example.test/', { apiPrefix: '/test' });
    t.after(() => client.close());
    return { client, streams, requests, diagnostics, MockEventSource };
}

test('connect shares one attempt and resolves on hello, without opening duplicate streams', async (t) => {
    const { client, streams } = setup(t);
    let opens = 0;
    client.onOpen(() => ++opens);
    const first = client.connect();
    assert.equal(client.connect(), first);
    assert.equal(streams.length, 1);
    assert.equal(streams[0].url, 'http://example.test/test/stream');
    assert.equal(client.isConnected(), false);
    streams[0].emit({ type: 'hello', protocol: 2 });
    await first;
    await client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    assert.equal(client.isConnected(), true);
    assert.equal(opens, 1);
    assert.equal(streams.length, 1);
});

test('close rejects a pending attempt and stale callbacks cannot affect a new stream', async (t) => {
    const { client, streams } = setup(t);
    const first = client.connect();
    const rejected = assert.rejects(first, /connection closed/);
    const oldMessage = streams[0].onmessage;
    const oldError = streams[0].onerror;
    client.close();
    client.close();
    await rejected;
    assert.equal(streams[0].closeCount, 1);

    const next = client.connect();
    oldMessage({ data: '{"type":"hello","protocol":2}' });
    oldMessage({ data: '{"type":"state","view":"old","field":"int","value":99}' });
    oldError({ type: 'error' });
    assert.equal(client.isConnected(), false);
    assert.equal(client.getState('old'), undefined);
    assert.equal(client.connect(), next);
    streams[1].emit({ type: 'hello', protocol: 2 });
    await next;
    oldError({ type: 'error' });
    assert.equal(client.isConnected(), true);
});

test('transient errors reject pending promises while native reconnect keeps one EventSource', async (t) => {
    const { client, streams } = setup(t);
    let opens = 0;
    let errors = 0;
    client.onOpen(() => ++opens);
    client.onError(() => ++errors);
    const first = client.connect();
    const rejected = assert.rejects(first, /connection failed/);
    streams[0].fail();
    await rejected;
    const second = client.connect();
    assert.notEqual(second, first);
    assert.equal(client.connect(), second);
    assert.equal(streams.length, 1);
    streams[0].emit({ type: 'hello', protocol: 2 });
    await second;
    assert.equal(opens, 1);

    streams[0].fail();
    assert.equal(client.isConnected(), false);
    streams[0].emit({ type: 'hello', protocol: 2 });
    assert.equal(client.isConnected(), true);
    assert.equal(opens, 2);
    assert.equal(errors, 2);
    assert.equal(streams[0].closeCount, 0);
});

test('connect after a live disconnect waits for the reconnect hello and close rejects it', async (t) => {
    const { client, streams } = setup(t);
    const first = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await first;
    streams[0].fail();
    const next = client.connect();
    assert.equal(client.connect(), next);
    const rejected = assert.rejects(next, /connection closed/);
    client.close();
    await rejected;
    assert.equal(streams.length, 1);
});

test('terminal errors discard the closed stream so retry creates a new one', async (t) => {
    const { client, streams } = setup(t);
    const first = client.connect();
    const rejected = assert.rejects(first, /connection failed/);
    streams[0].fail(true);
    await rejected;
    const next = client.connect();
    assert.equal(streams.length, 2);
    assert.equal(streams[0].closeCount, 1);
    streams[0].emit({ type: 'hello', protocol: 2 });
    assert.equal(client.isConnected(), false);
    streams[1].emit({ type: 'hello', protocol: 2 });
    await next;
});

test('EventSource construction failure rejects and permits retry', async (t) => {
    const { client, streams, MockEventSource } = setup(t);
    MockEventSource.failConstruction = true;
    await assert.rejects(client.connect(), /constructor failed/);
    MockEventSource.failConstruction = false;
    const retry = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await retry;
});

test('lifecycle callback exceptions cannot prevent settlement or other callbacks', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const notifications = [];
    client.onOpen(() => { throw new Error('open subscriber'); });
    client.onOpen(() => notifications.push('open'));
    client.onError(() => { throw new Error('error subscriber'); });
    client.onError(() => notifications.push('error'));
    const first = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await first;
    streams[0].fail();
    const reconnect = client.connect();
    const rejected = assert.rejects(reconnect, /connection failed/);
    streams[0].fail();
    await rejected;
    streams[0].emit({ type: 'hello', protocol: 2 });
    assert.deepEqual(notifications, ['open', 'error', 'error', 'open']);
    assert.equal(diagnostics.length, 4);
    assert.equal(client.isConnected(), true);
});

test('onOpen can close synchronously without leaving the promise pending or state connected', async (t) => {
    const { client, streams } = setup(t);
    let secondCalled = false;
    client.onOpen(() => client.close());
    client.onOpen(() => { secondCalled = true; });
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    assert.equal(secondCalled, true);
    assert.equal(client.isConnected(), false);
});

test('subscriber exceptions are isolated for values, clicks, visibility and enabled', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const received = [];
    const fail = () => { throw new Error('subscriber'); };
    client.subscribe('value', fail);
    const unsubscribe = client.subscribe('value', (value) => received.push(value));
    client.onEvent('value', fail);
    client.onEvent('value', () => received.push('click'));
    for (const field of ['visibility', 'enabled']) {
        client.subscribe('value', fail, field);
        client.subscribe('value', (value) => received.push([field, value]), field);
    }
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    streams[0].emit({ type: 'state', view: 'value', field: 'text', value: 'hello' });
    streams[0].emit({ type: 'event', view: 'value', field: 'click' });
    streams[0].emit({ type: 'visibility', view: 'value', value: false });
    streams[0].emit({ type: 'enabled', view: 'value', value: true });
    assert.deepEqual(received, ['hello', 'click', ['visibility', false], ['enabled', true]]);
    assert.equal(client.getState('value', 'visibility'), false);
    assert.equal(client.getState('value', 'enabled'), true);
    assert.equal(diagnostics.length, 4);
    unsubscribe();
    streams[0].emit({ type: 'state', view: 'value', field: 'text', value: 'later' });
    assert.equal(received.length, 4);
    assert.equal(client.getState('value'), 'later');
});

test('protocol 2 decodes exact int64 and uint64 values to safe Number or BigInt', async (t) => {
    const { client, streams } = setup(t);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    const cases = [
        ['int64', -9007199254740991, -9007199254740991],
        ['int64', 9007199254740991, 9007199254740991],
        ['int64', '9007199254740991', 9007199254740991],
        ['int64', '9007199254740992', 9007199254740992n],
        ['int64', '-9007199254740992', -9007199254740992n],
        ['int64', '-9223372036854775808', -9223372036854775808n],
        ['int64', '9223372036854775807', 9223372036854775807n],
        ['uint64', 0, 0],
        ['uint64', '00042', 42],
        ['uint64', '18446744073709551615', 18446744073709551615n],
    ];
    const values = [];
    client.subscribe('value', (value) => values.push(value));
    for (const [field, wire, expected] of cases) {
        streams[0].emit({ type: 'state', view: 'value', field, value: wire });
        assert.equal(client.getState('value'), expected);
        assert.equal(values.at(-1), expected);
    }
});

test('unset int64 and uint64 snapshots preserve null and notify subscribers', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    for (const field of ['int64', 'uint64']) {
        const values = [];
        client.subscribe(field, (value) => values.push(value));
        assert.equal(client.getState(field), undefined);
        streams[0].emit({ type: 'state', view: field, field, value: null });
        assert.equal(client.getState(field), null);
        assert.deepEqual(values, [null]);
    }
    assert.equal(diagnostics.length, 0);
});

test('null reset snapshots replace cached int64 and uint64 BigInt values', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    for (const [field, large] of [
        ['int64', -9223372036854775808n],
        ['uint64', 18446744073709551615n],
    ]) {
        const values = [];
        client.subscribe(field, (value) => values.push(value));
        streams[0].emit({ type: 'state', view: field, field, value: large.toString() });
        assert.equal(client.getState(field), large);
        streams[0].emit({ type: 'state', view: field, field, value: null });
        assert.equal(client.getState(field), null);
        assert.deepEqual(values, [large, null]);
    }
    assert.equal(diagnostics.length, 0);
});

test('protocol 1 keeps safe numbers but rejects already-rounded numeric events', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 1 });
    await connected;
    streams[0].emit({ type: 'state', view: 'value', field: 'int64', value: 42 });
    streams[0].onmessage({ data: '{"type":"state","view":"value","field":"int64","value":9007199254740993}' });
    assert.equal(client.getState('value'), 42);
    assert.equal(diagnostics.length, 1);
    assert.match(diagnostics[0][2].message, /safe integer/);
});

test('invalid integer envelopes leave the last valid state and subscribers unchanged', async (t) => {
    const { client, streams, diagnostics } = setup(t);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    let notifications = 0;
    client.subscribe('value', () => ++notifications);
    streams[0].emit({ type: 'state', view: 'value', field: 'int64', value: 7 });
    const invalid = [
        ['int64', 9007199254740992], ['int64', 1.5], ['int64', true],
        ['int64', {}], ['int64', ''], ['int64', '1e3'],
        ['int64', '+1'], ['int64', ' 1'], ['int64', '1\n'], ['int64', '1.0'],
        ['int64', '9223372036854775808'], ['int64', '-9223372036854775809'],
        ['uint64', -1], ['uint64', '-0'], ['uint64', '18446744073709551616'],
    ];
    for (const [field, value] of invalid) streams[0].emit({ type: 'state', view: 'value', field, value });
    assert.equal(client.getState('value'), 7);
    assert.equal(notifications, 1);
    assert.equal(diagnostics.length, invalid.length);
});

test('64-bit setters serialize exact boundaries and normalize safe integers', async (t) => {
    const { client, requests } = setup(t);
    const cases = [
        ['setInt64', -9007199254740991, -9007199254740991],
        ['setInt64', 42n, 42],
        ['setInt64', '9007199254740991', 9007199254740991],
        ['setInt64', 9007199254740992n, '9007199254740992'],
        ['setInt64', '-9223372036854775808', '-9223372036854775808'],
        ['setInt64', 9223372036854775807n, '9223372036854775807'],
        ['setUInt64', '00042', 42],
        ['setUInt64', 0n, 0],
        ['setUInt64', 18446744073709551615n, '18446744073709551615'],
    ];
    for (const [method, value, wire] of cases) {
        await client[method]('value', value);
        const request = requests.at(-1);
        assert.equal(request.url, 'http://example.test/test/state');
        assert.deepEqual(JSON.parse(request.options.body), {
            view: 'value', field: method === 'setInt64' ? 'int64' : 'uint64', value: wire,
        });
    }
});

test('64-bit setters reject unsafe, wrong-type and out-of-range input before fetch', async (t) => {
    const { client, requests } = setup(t);
    for (const value of [9007199254740992, NaN, Infinity, 1.25, true, null, {}, [], '', '1e3', '+1', ' 1', '1\n', '1.0']) {
        await assert.rejects(client.setInt64('value', value), /requires/);
        await assert.rejects(client.setUInt64('value', value), /requires/);
    }
    for (const value of [9223372036854775808n, '-9223372036854775809']) {
        await assert.rejects(client.setInt64('value', value), /out of range/);
    }
    for (const value of [-1, -1n, 18446744073709551616n]) {
        await assert.rejects(client.setUInt64('value', value), /out of range/);
    }
    await assert.rejects(client.setUInt64('value', '-1'), /decimal/);
    assert.equal(requests.length, 0);
});

test('all fetch helpers reject non-success status with the endpoint and response detail', async (t) => {
    const { client } = setup(t);
    t.mock.method(globalThis, 'fetch', async () => new Response('{"error":"view missing"}', { status: 404 }));
    const calls = [
        () => client.setText('x', 'v'), () => client.setBool('x', true),
        () => client.setInt('x', 1), () => client.setInt64('x', 1n),
        () => client.setUInt64('x', 1n), () => client.setFloat('x', 1),
        () => client.setDouble('x', 1), () => client.click('x'),
        () => client.command('x', 'save'), () => client.listViews(), () => client.health(),
    ];
    for (const call of calls) await assert.rejects(call(), /Aria HTTP 404 \/\w+: .*view missing/);
});

test('successful fetch helpers keep their JSON results and request bodies', async (t) => {
    const { client, requests } = setup(t);
    assert.deepEqual(await client.listViews(), [{ id: 'value', kind: 'int64' }]);
    assert.equal((await client.health()).ok, true);
    await client.click('button');
    assert.deepEqual(JSON.parse(requests.at(-1).options.body), { view: 'button' });
    await client.command('vm', 'save', { value: 7 });
    assert.deepEqual(JSON.parse(requests.at(-1).options.body), { view: 'vm', command: 'save', args: { value: 7 } });
});

test('state channels keep dotted view IDs distinct from visibility and enabled', async (t) => {
    const { client, streams } = setup(t);
    const values = [];
    client.subscribe('item.enabled', value => values.push(['value', value]));
    client.subscribe('item', value => values.push(['enabled', value]), 'enabled');
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    streams[0].emit({ type: 'state', view: 'item.enabled', field: 'text', value: 'label' });
    streams[0].emit({ type: 'enabled', view: 'item', value: false });
    assert.equal(client.getState('item.enabled'), 'label');
    assert.equal(client.getState('item', 'enabled'), false);
    assert.deepEqual(values, [['value', 'label'], ['enabled', false]]);
});

test('unsubscribe is idempotent across callback reuse and empty buckets are released', async (t) => {
    const { client, streams } = setup(t);
    const values = [];
    const callback = value => values.push(value);
    const keepBucket = client.subscribe('value', () => {});
    const stopFirst = client.subscribe('value', callback);
    stopFirst();
    const stopSecond = client.subscribe('value', callback);
    stopFirst();
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    streams[0].emit({ type: 'state', view: 'value', field: 'int', value: 7 });
    assert.deepEqual(values, [7]);
    stopSecond();
    keepBucket();
    const stopEvent = client.onEvent('transient', () => {});
    stopEvent();
    assert.equal(client._eventSubs.has('transient'), false);
});

test('duplicate callbacks own separate registrations and cancellation skips pending delivery', async (t) => {
    const { client, streams } = setup(t);
    let calls = 0;
    const callback = () => ++calls;
    const first = client.subscribe('value', callback);
    const second = client.subscribe('value', callback);
    const connected = client.connect();
    streams[0].emit({ type: 'hello', protocol: 2 });
    await connected;
    streams[0].emit({ type: 'state', view: 'value', field: 'int', value: 1 });
    assert.equal(calls, 2);
    first();
    streams[0].emit({ type: 'state', view: 'value', field: 'int', value: 2 });
    assert.equal(calls, 3);
    second();
    let cancelPending;
    const cancelFirst = client.subscribe('value', () => cancelPending());
    cancelPending = client.subscribe('value', callback);
    streams[0].emit({ type: 'state', view: 'value', field: 'int', value: 3 });
    assert.equal(calls, 3);
    cancelFirst();
    assert.equal(client._stateChannels.get('value').subscribers.size, 0);
    assert.throws(() => client.subscribe('value', callback, 'invalid'), /state field/);
    assert.throws(() => client.getState('value', 'invalid'), /state field/);
    assert.throws(() => client.subscribe('value', null), /subscriber/);
});
