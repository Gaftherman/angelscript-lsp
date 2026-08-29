import io, re

FIXTURE = 'tests/fixtures/full-addons.as.predefined'
SDK = 'tests/fixtures/sdk-addons.as.predefined'

text = io.open(FIXTURE, encoding='utf-8').read()

# ---------------------------------------------------------------- 1. correct the socket section
# scriptsocket IS an SDK add-on (sdk/add_on/scriptsocket), and its API is far smaller than the one
# invented here: no address family enums, no bind, and connect takes a packed IPv4 address rather
# than a host string.
start = text.index('// =====================================================================================\n// socket')
end = text.index('// =====================================================================================\n// String utilities')

corrected_socket = '''// =====================================================================================
// socket - add_on/scriptsocket/scriptsocket.cpp
//
// Registered by the SDK, but not by AS-Harness and not by this repository's parity oracle: it needs
// platform socket libraries, which is a poor trade for a CI job whose purpose is to compile scripts.
// The declarations are taken from the add-on's own RegisterObjectMethod calls.
//
// Note `connect` takes a packed IPv4 address as a uint, not a host string.
// =====================================================================================

class socket
{
    socket();
    /// Binds and starts listening on the given port. Returns a negative value on failure.
    int listen(uint16 port);
    int close();
    /// Waits up to `timeout` milliseconds for a client, or null if none arrived.
    socket@ accept(int64 timeout = 0);
    int connect(uint ipv4address, uint16 port);
    /// Sends bytes; returns the number written, or a negative value on error.
    int send(const string &in data);
    /// Reads whatever has arrived, waiting up to `timeout` milliseconds.
    string receive(int64 timeout = 0);
    bool isActive() const;
}


'''
text = text[:start] + corrected_socket + text[end:]
io.open(FIXTURE, 'w', encoding='utf-8', newline='').write(text)
print('fixture socket section corrected')

# ---------------------------------------------------------------- 2. derive the oracle's stub
def drop_section(body, heading):
    begin = body.index('// =====================================================================================\n// ' + heading)
    rest = body.index('// =====================================================================================\n// ', begin + 90)
    return body[:begin] + body[rest:]

sdk = text

# contextmgr and scriptsocket are not compiled into the oracle, so nothing may declare them.
sdk = drop_section(sdk, 'Co-routines')
sdk = drop_section(sdk, 'socket')

# The Exception object, SetException and GetException come from AS-Harness's own add-ons, not the
# SDK. `throw` / `getExceptionInfo` are scripthelper's and stay.
exc_begin = sdk.index('/// Addon: ASException\n/// Exception object')
exc_end = sdk.index('/// add_on/scripthelper/scripthelper.cpp - raises a script exception.')
sdk = sdk[:exc_begin] + sdk[exc_end:]

header_end = sdk.index('\n\n// =====================================================================================\n// string -')
sdk = '''// The exact script surface server/tools/oracle/main.cpp registers.
//
// GENERATED from full-addons.as.predefined - do not edit by hand. Re-run
// scripts/make-sdk-stub.py after changing either this or the oracle's registrations.
//
// The parity audit compares this analyzer against a real compiler, and that comparison is only
// honest when both see the same API. This stub is therefore paired with the oracle: everything the
// oracle registers is declared here, and nothing else is. Types AS-Harness adds on top of the SDK -
// its Exception object, optional<T>, JSON - are deliberately absent, as are the add-ons the oracle
// does not compile (contextmgr's co-routines, scriptsocket).
//
// See server/cmake/Oracle.cmake for the add-on list, and the README section "Initializer List
// Patterns" for what the @listpattern tags do.
''' + sdk[header_end:]

io.open(SDK, 'w', encoding='utf-8', newline='').write(sdk)
print('sdk stub written:', sum(1 for _ in io.open(SDK, encoding='utf-8')), 'lines')

for forbidden in ['class Exception', 'SetException', 'GetException', 'createCoRoutine', 'class socket', 'yield()']:
    if forbidden in sdk:
        raise SystemExit('sdk stub still declares ' + forbidden)
for required in ['class array<T>', 'class grid<T>', 'class dictionary', 'class weakref<T>',
                 'class any', 'class complex', 'void throw(', 'string getExceptionInfo()']:
    if required not in sdk:
        raise SystemExit('sdk stub is missing ' + required)
print('content checks passed')
