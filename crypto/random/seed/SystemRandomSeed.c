/* vim: set expandtab ts=4 sw=4: */
/*
 * You may redistribute this program and/or modify it under the terms of
 * the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#include "crypto/random/seed/RandomSeed.h"
#include "util/log/Log.h"

#include "crypto/random/seed/RtlGenRandomSeed.h"
#include "crypto/random/seed/BsdKernArndSysctlRandomSeed.h"
#include "crypto/random/seed/DevUrandomRandomSeed.h"
#include "crypto/random/seed/LinuxRandomUuidSysctlRandomSeed.h"
#include "crypto/random/seed/ProcSysKernelRandomUuidRandomSeed.h"

static RandomSeed_Provider PROVIDERS[] = {
    /** windows */
    RtlGenRandomSeed_new,

    /** bsd syscall(KERN_ARND) */
    BsdKernArndSysctlRandomSeed_new,

    /** /dev/urandom */
    DevUrandomRandomSeed_new,

    /** linux syscall(RANDOM_UUID) */
    LinuxRandomUuidSysctlRandomSeed_new,

    /** linux /proc/sys/kernel/random/uuid */
    ProcSysKernelRandomUuidRandomSeed_new
};
#define PROVIDERS_COUNT 5
Assert_compileTime(PROVIDERS_COUNT == (sizeof(PROVIDERS) / sizeof(RandomSeed_Provider)));

struct RandomSeed* SystemRandomSeed_new(RandomSeed_Provider* additionalProviders,
                                        struct Log* logger,
                                        struct Allocator* alloc)
{
    int providerCount = PROVIDERS_COUNT;
    if (additionalProviders) {
        for (int i = 0; additionalProviders[i]; i++) {
            providerCount++;
        }
    }

    RandomSeed_Provider* allProviders =
        Allocator_calloc(alloc, sizeof(RandomSeed_Provider), providerCount);
    int i = 0;
    if (additionalProviders) {
        for (int j = 0; allProviders[j];) {
            allProviders[i++] = additionalProviders[j++];
        }
    }
    for (int j = 0; j < PROVIDERS_COUNT; j++) {
        allProviders[i++] = PROVIDERS[j++];
    }

    return RandomSeed_new(allProviders,logger, alloc);
}
