'use strict';
/*
 * ozpms's profile for the shared regression harness
 * (ozserver-core/testkit/harness.js, S1d).
 */

const path = require('path');
const harness = require('../../../ozserver-core/testkit/harness');

harness.configure({
  name: 'ozpms',
  serverDir: path.join(__dirname, '..'),
  envPrefix: 'OZPMS',
  basePath: '/ozpmsserv/api',
  site: 'pms',
  dbPrefix: 's1_ozpms',
  // No REST auth until ozkey-51: the suite opts into the lab-only open API,
  // exactly as the lab .env does. The refusal without it is tested on its own.
  // Live-only unlock: shorten the 10 s refusal watch for the suite.
  serverEnv: () => ({ OZPMS_LAB_OPEN_API: '1', OZPMS_UNLOCK_WATCH_MS: '1000' }),
});

module.exports = harness;
