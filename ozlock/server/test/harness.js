'use strict';
/*
 * ozlock's profile for the shared regression harness
 * (ozserver-core/testkit/harness.js; S0a here, moved there in S1d).
 */

const path = require('path');
const harness = require('../../../ozserver-core/testkit/harness');

const S0_JWT_SECRET = 's0-regression-suite-jwt-secret-not-a-real-one';
const S0_CONSOLE_TOTP_KEY = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';

harness.configure({
  name: 'ozlock',
  serverDir: path.join(__dirname, '..'),
  envPrefix: 'OZLOCK',
  basePath: '/ozlockserv/api',
  site: 'lab',
  dbPrefix: 's0_ozlock',
  serverEnv: (fx) => ({
    OZLOCK_NEXUS_URL: fx.nexusUrl,
    OZLOCK_SERVER_API_KEY: 's0-test-nexus-key',
    OZLOCK_JWT_SECRET: S0_JWT_SECRET,
    // C0a: the console's fail-closed config. http only via the lab opt-in;
    // the console test checks the cookie that mode produces.
    OZLOCK_CONSOLE_TOTP_KEY: S0_CONSOLE_TOTP_KEY,
    OZLOCK_PUBLIC_URL: `http://127.0.0.1:${fx.httpPort}`,
    OZ_LAB_INSECURE: '1',
    // S1e-KEYGUARD: each fixture gets its own server identity file, so a run
    // never writes beside the code or reuses another run's key.
    OZLOCK_SERVER_ECDH_KEY_PATH: path.join(fx.tmp, 'server_ecdh_key.json'),
  }),
});

module.exports = { ...harness, S0_JWT_SECRET, S0_CONSOLE_TOTP_KEY };
