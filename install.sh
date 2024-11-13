cd api
make && sudo make install
cd ..
make config
make && doas make install

echo "Successfully installed ragnarwm."
