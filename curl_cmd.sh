curl -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode "a=YOUR_ADMIN_KEY" \
  --data-urlencode "csv@/path/to/villas_108_import.csv"


curl -sS -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode 'a=KxQmNpRjWt' \
  --data-urlencode 'csv=9908195316,a1b2c3d4e5f6,74,""'

curl -sS -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode 'a=YOUR_ADMIN_TOKEN' \
  --data-urlencode 'csv@/home/ravikumar/gate-controller/villas_108_import.csv'

curl -sS 'http://192.168.4.1/adduser?a=KxQmNpRjWt' | jq .


curl -X POST 'http://192.168.4.1/adduser/bulk' \
  --data-urlencode "a=KxQmNpRjWt" \
  --data-urlencode "csv@/home/ravikumar/gate-controller/residents.csv"
