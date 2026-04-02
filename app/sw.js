self.addEventListener("install", e => {
 e.waitUntil(
  caches.open("gate-app-v3").then(cache => {
    return cache.addAll(["./","index.html","manifest.json"]);
  })
 );
});
