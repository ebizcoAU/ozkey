'use strict';
/* S1e-4: pre-UTC DATETIME purge migration on the ozlock profile (shared suite). */
const harness = require('./harness');
require('../../../ozserver-core/testkit/utcmigration')(harness);
