# SGI L2/L3 Controller software for Modern Linux/Docker


Contained in this repo is a solution for setting up SGI's L3 emulator software on a modern linux distribution (Debian 13) or in a docker container.  
AI was used to help make this.

There are 3 directories in the repo. Which ones you use depends on how you are deploying it.


## Usage
Both the docker container and the services run the `l2` application in the background. A new user called "l2" is created. The shell for this user is `l2term`.

To access, use telnet to connect to the host the application is running on. 
You are automatically logged in as "l2" and running the `l2term` program.

To logout, press `CTRL-]`, then `q`.

You can connect with `l2gui`, `l2cmd`, or `l2term` from a different computer by passing the `--l2` parameter with the IP address of the l2 server.

If you installed the bare metal services version, you can also ssh to `l2@host` rather than using telnet.


## Deployment Scenarios
The driver lives wherever the kernel runs; the application lives wherever it's executed. Match the file sets accordingly.

| Where it lives                                     | Driver runs on …            | Application runs on …                |
| -------------------------------------------------- | --------------------------- | ------------------------------------ |
| Bare-metal Debian 13                               | the host (`sgil1_driver/`)        | the host (`bare_metal_services/`)               |
| Proxmox VM with USB passthrough into the VM        | inside the VM (`sgil1_driver/`)   | inside the VM (`bare_metal_services/`)          |
| Docker container                                   | the Docker host (`sgil1_driver/`) | inside the container (`docker/`)     |


### Running in a VM
If you are running in a VM, either docker or the bare metal service, you must passthrough the SGI L1 device to the VM.
In proxmox, Passthrough the USB device by Vendor/Device ID.  
At least for an Altix 350, the Vendor/Device is `065e:1234`



### Docker on VM or Bare Metal
You will use the `sgil1_driver` and `docker` directories.

See [Docker readme](https://github.com/nsafran1217/SGI-L2-Docker/blob/main/docker/README.md) for more information. This includes information about troubleshooting, and service management.

Clone repo: 
```
git clone https://github.com/nsafran1217/SGI-L2-Docker.git
cd SGI-L2-Docker
```

Install the driver:

```
cd sgil1_driver
./setup-driver.sh
```

Setup the docker container:

```
cd ../docker
docker compose build
docker compose up -d
```

### Bare Metal or VM
You will use the `sgil1_driver` and `bare_metal_services` directories.  
*the service setup script has only been tested on Debian 13. It will not work on non-debian based distros*

See [bare metal readme](https://github.com/nsafran1217/SGI-L2-Docker/blob/main/bare_metal_services/README.md) for more information.

Clone repo: 
```
git clone https://github.com/nsafran1217/SGI-L2-Docker.git
cd SGI-L2-Docker
```

Install the driver:

```
cd sgil1_driver
./setup-driver.sh
```

Setup the services and users:

```
cd ../bare_metal_services
./setup-services.sh ../snxsc_l3.tar.gz
```

## More information

The setup-services.sh script:
installs multilib i386, installs required libraries, extracts l3 software to /opt/snxsc_l3, sets up telnet, sets up a systemd service for l2, creates the l2 user.

If you wanted to use the L3 software without relying on the setup scripts and installing it as a service, you can just install the i386 libs and extract the files into root and use them as SGI intended.


```
sudo dpkg --add-architecture i386
sudo apt update
sudo apt install libc6:i386 libcrypt1:i386 libx11-6:i386
cd /
sudo tar xf /path/to/snxsc_l3.tar.gz
```
Then modify your path to include `/stand/sysco/bin` and run the tools as needed.

I still recommend running setup-driver.sh if you are on a debian based distro. If not, you can manually install the driver by running `make && sudo make install` in `sgil1_driver/src`




The files located in snxsc_l3.tar.gz are copyright SGI. They were extracted from `snxsc_l3-1.62.0-1.i386.rpm` from the ist-3.24 CD.