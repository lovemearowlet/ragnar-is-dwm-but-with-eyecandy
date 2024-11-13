cd api
make && doas make install
cd ..
make config
make && doas make install

echo "Successfully installed ragnarwm."
