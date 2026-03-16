// Copyright (c) 2017-2023 Cloudflare, Inc.
// Licensed under the Apache 2.0 license found in the LICENSE file or at:
//     https://opensource.org/licenses/Apache-2.0

// Sieve of Eratosthenes — heavy top-level computation.
const N = 500_000_000;
const sieve = new Uint8Array(N + 1).fill(1);
sieve[0] = sieve[1] = 0;
for (let i = 2; i * i <= N; ++i) {
  if (sieve[i]) {
    for (let j = i * i; j <= N; j += i) sieve[j] = 0;
  }
}

let primeCount = 0;
for (let i = 0; i <= N; ++i) primeCount += sieve[i];

addEventListener('fetch', event => {
  event.respondWith(handle(event.request));
});

async function handle(request) {
  return new Response(`Hello World: primes up to ${N} = ${primeCount}`);
}
