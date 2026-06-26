```sql
mysql -h localhost -P 3306 -u wenumseveld -p   --batch --raw  -e "SELECT dateTime, airTemperature, airHumidity, lumosity FROM wenumseveld WHERE sensor='lht65-20' AND dateTime >= '2026-03-17' AND dateTime < '2026-05-07';"   wenumseveld | sed 's/\t/,/g' > greenhouseClimate-lht65-20_2026-03-17_to_2026-05-07.csv
```

Sensors:
LHT65-03	in kas 1
LHT65-02	in kas 2
lht65-20	buiten kas
