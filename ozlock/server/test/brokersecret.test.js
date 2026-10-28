'use strict';
/* S1e-1: broker-secret boot migration on the ozlock profile (shared suite). */
const harness = require('./harness');
require('../../../ozserver-core/testkit/brokersecret')(harness, { hashLocks: false });
