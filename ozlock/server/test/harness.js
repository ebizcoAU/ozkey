'use strict';
/*
 * ozlock's profile for the shared regression harness
 * (ozserver-core/testkit/harness.js; S0a here, moved there in S1d).
 */

const path = require('path');
const harness = require('../../../ozserver-core/testkit/harness');

const S0_JWT_SECRET = 's0-regression-suite-jwt-secret-not-a-real-one';

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
  }),
});

module.exports = { ...harness, S0_JWT_SECRET };
