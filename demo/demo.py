#!/usr/bin/env python

#
# Author: Vincenzo Maffione <v.maffione@gmail.com>
#

import multiprocessing
import subprocess
import argparse
import json
import copy
import re
import os


def download_if_needed(locpath, url):
    try:
        import urllib.request
        downloader = urllib.request
    except:
        import urllib
        downloader = urllib

    if os.path.isfile(locpath):
        return

    print('Downloading %s ...' % url)
    fin = downloader.urlretrieve(url, locpath)
    print('... download complete')


def which(program):
    FNULL = open(os.devnull, 'w')
    retcode = subprocess.call(['sudo', 'which', program], stdout = FNULL,
                              stderr = subprocess.STDOUT)
    if retcode != 0:
        print('Fatal error: Cannot find "%s" program' % program)
        quit(1)


def join_keys(d1, d2):
    k = []
    for d in d1:
        k.append(d)
    for d in d2:
        k.append(d)
    return k


# @prefix is a string
def prefix_parse(prefix):
    m = re.match('^(\d+)\.(\d+)\.(\d+)\.(\d+)/(\d+)$', prefix)
    if not m:
        return None, None, None

    a = int(m.group(1))
    b = int(m.group(2))
    c = int(m.group(3))
    d = int(m.group(4))
    m = int(m.group(5))

    v = lambda x: x >= 0 and x <= 255
    if not v(a) or not v(b) or not v(c) or not v(d) or m < 0 or m > 32:
        return None, None, None

    num = (a << 24) + (b << 16) + (c << 8) + d
    mask = ((1 << m) - 1) << (32 - m)

    return (num, mask, m)


# @prefix is a string
def prefix_is_valid(prefix):
    num, mask, size = prefix_parse(prefix)

    if num == None or mask == None:
        return False

    return True


def prefix_prune_size(prefix):
    return prefix[:prefix.rfind('/')]


def num_to_ip(num, size, nomask):
    a = (num >> 24) & 0xFF
    b = (num >> 16) & 0xFF
    c = (num >> 8)  & 0xFF
    d = (num >> 0)  & 0xFF

    if nomask:
        return '%s.%s.%s.%s' % (a, b, c, d)

    return '%s.%s.%s.%s/%s' % (a, b, c, d, size)


def ip_in_prefix(prefix, i, nomask = False):
    num, mask, size = prefix_parse(prefix)
    if num == None or mask == None:
        return None

    if i & mask != 0:
        return None

    ip_num = num + i

    return num_to_ip(ip_num, size, nomask)

description = "Python script to generate rlite deployments based on light VMs"
epilog = "2015-2016 Vincenzo Maffione <v.maffione@gmail.com>"

argparser = argparse.ArgumentParser(description = description,
                                    epilog = epilog)
argparser.add_argument('-c', '--conf',
                       help = "demo.conf configuration file", type = str,
                       default = 'demo.conf')
argparser.add_argument('-g', '--graphviz', action='store_true',
                       help = "Generate DIF graphs with graphviz")
argparser.add_argument('-m', '--memory',
                       help = "Amount of memory in megabytes", type = int,
                       default = 128)
argparser.add_argument('-p', '--base-port',
                       help = "Base SSH port to map nodes", type = int,
                       default = 2222)
argparser.add_argument('-e', '--enrollment-strategy',
                       help = "Minimal uses a spanning tree of each DIF",
                       type = str, choices = ['minimal', 'full-mesh'],
                       default = 'minimal')
argparser.add_argument('--ring',
                       help = "Use ring topology with variable number of nodes",
                       type = int)
argparser.add_argument('--verbosity',
                       help = "Set verbosity level for kernel and userspace",
                       choices = ['VERY', 'DBG', 'INFO', 'WARN', 'QUIET'],
                       default = 'DBG')
argparser.add_argument('-f', '--frontend',
                       help = "Choose which emulated NIC the nodes will use",
                       type = str, choices = ['virtio-net-pci', 'e1000'],
                       default = 'virtio-net-pci')
argparser.add_argument('--vhost', action='store_true',
                       help = "Use vhost acceleration for virtio-net frontend")
args = argparser.parse_args()


# Check we have what we need
which('brctl')
which('qemu-system-x86_64')

subprocess.call(['chmod', '0400', 'buildroot/buildroot_rsa'])

# Some variables that could become options
sshopts = '-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null '\
          '-o IdentityFile=buildroot/buildroot_rsa'
sudo = ''
vmimgpath = 'buildroot/rootfs.cpio'
username = 'root'

download_if_needed(vmimgpath, 'https://bitbucket.org/vmaffione/rlite-images/downloads/rootfs.cpio')
download_if_needed('buildroot/bzImage', 'https://bitbucket.org/vmaffione/rlite-images/downloads/bzImage')

# Possibly autogenerate ring topology
if args.ring != None and args.ring > 0:
    print("Ignoring %s, generating ring topology" % (args.conf,))
    fout = open('ring.conf', 'w')
    for i in range(args.ring):
        i_next = i + 1
        if i_next == args.ring:
            i_next = 0
        fout.write('eth b%(i)s 0Mbps m%(i)03d m%(inext)03d\n' % {'i': i+1,
                                                           'inext': i_next+1})
    for i in range(args.ring):
        i_prev = i - 1
        if i_prev < 0:
            i_prev = args.ring - 1
        fout.write('dif n m%(i)03d b%(i)s b%(iprev)s\n' % {'i': i+1,
                                                         'iprev': i_prev+1})
    fout.close()
    args.conf = 'ring.conf'


############################# Parse demo.conf ##############################
fin = open(args.conf, 'r')

vms = dict()
shims = dict()
links = []
difs = dict()
enrollments = dict()
lowerflowallocs = dict()
dif_graphs = dict()
dns_mappings = dict()

linecnt = 0

while 1:
    line = fin.readline()
    if line == '':
        break
    linecnt += 1

    line = line.replace('\n', '')

    if line.startswith('#'):
        continue

    m = re.match(r'\s*eth\s+(\w+)\s+(\d+)([GMK])bps\s+(\w.*)$', line)
    if m:
        shim = m.group(1)
        speed = int(m.group(2))
        speed_unit = m.group(3).lower()
        vm_list = m.group(4).split()

        if shim in shims:
            print('Error: Line %d: shim %s already defined' \
                                            % (linecnt, shim))
            continue

        shims[shim] = {'name': shim, 'speed': speed,
                       'speed_unit': speed_unit, 'type': 'eth'}

        for vm in vm_list:
            if vm not in vms:
                vms[vm] = {'name': vm, 'ports': []}
            links.append((shim, vm))

        #for i in range(len(vm_list)-1):
        #    for j in range(i + 1, len(vm_list)):
        #        print(vm_list[i], vm_list[j])
        continue

    m = re.match(r'\s*udp4\s+(\w+)\s+(\w.*)$', line)
    if m:
        shim = m.group(1)
        members = m.group(2).split()

        if shim in shims:
            print('Error: Line %d: udp4 %s already defined' \
                                            % (linecnt, shim))
            continue

        shims[shim] = {'name': shim, 'type': 'udp4'}

        dns_mappings[shim] = dict()

        for member in members:
            vm, ip = member.split(':')

            if not prefix_is_valid(ip):
                print('Error: Line %d: ip %s is not valid' \
                                                % (linecnt, ip))
                continue

            if vm not in vms:
                vms[vm] = {'name': vm, 'ports': []}
            links.append((shim, vm))
            dns_mappings[shim][vm] = {'ip': ip}
        continue

    m = re.match(r'\s*dif\s+(\w+)\s+(\w+)\s+(\w.*)$', line)
    if m:
        dif = m.group(1)
        vm = m.group(2)
        dif_list = m.group(3).split()

        if vm not in vms:
            vms[vm] = {'name': vm, 'ports': []}

        if dif not in difs:
            difs[dif] = dict()

        if vm in difs[dif]:
            print('Error: Line %d: vm %s in dif %s already specified' \
                                            % (linecnt, vm, dif))
            continue

        difs[dif][vm] = dif_list

        continue

fin.close()

boot_batch_size = max(1, multiprocessing.cpu_count() / 2)
wait_for_boot = 12  # in seconds
if len(vms) > 8:
    print("You want to run a lot of nodes, so it's better if I give "
          "each node some time to boot (since the boot is CPU-intensive) "
          "and a minimum amount of memory")
    args.memory = 128 # in megabytes

############ Compute registration/enrollment order for DIFs ###############

# Compute DIFs dependency graph, as both adjacency and incidence list.
difsdeps_adj = dict()
difsdeps_inc = dict()
for dif in difs:
    difsdeps_inc[dif] = set()
    difsdeps_adj[dif] = set()
for shim in shims:
    difsdeps_inc[shim] = set()
    difsdeps_adj[shim] = set()

for dif in difs:
    for vmname in difs[dif]:
        for lower_dif in difs[dif][vmname]:
            difsdeps_inc[dif].add(lower_dif)
            difsdeps_adj[lower_dif].add(dif)

# Kahn's algorithm below only needs per-node count of
# incident edges, so we compute these counts from the
# incidence list and drop the latter.
difsdeps_inc_cnt = dict()
for dif in difsdeps_inc:
    difsdeps_inc_cnt[dif] = len(difsdeps_inc[dif])
del difsdeps_inc

#print(difsdeps_adj)
#print(difsdeps_inc_inc)

# Run Kahn's algorithm to compute topological ordering on the DIFs graph.
frontier = set()
dif_ordering = []
for dif in difsdeps_inc_cnt:
    if difsdeps_inc_cnt[dif] == 0:
        frontier.add(dif)

while len(frontier):
    cur = frontier.pop()
    dif_ordering.append(cur)
    for nxt in difsdeps_adj[cur]:
        difsdeps_inc_cnt[nxt] -= 1
        if difsdeps_inc_cnt[nxt] == 0:
            frontier.add(nxt)
    difsdeps_adj[cur] = set()

circular_set = [dif for dif in difsdeps_inc_cnt if difsdeps_inc_cnt[dif] != 0]
if len(circular_set):
    print("Fatal error: The specified DIFs topology has one or more"\
          "circular dependencies, involving the following"\
          " DIFs: %s" % circular_set)
    print("             DIFs dependency graph: %s" % difsdeps_adj);
    quit(1)


####################### Compute DIF graphs #######################
for dif in difs:
    neighsets = dict()
    dif_graphs[dif] = dict()
    first = None

    # For each N-1-DIF supporting this DIF, compute the set of nodes that
    # share such N-1-DIF. This set will be called the 'neighset' of
    # the N-1-DIF for the current DIF.

    for vmname in difs[dif]:
        dif_graphs[dif][vmname] = [] # init for later use
        if first == None: # pick any node for later use
            first = vmname
        first = vmname
        for lower_dif in difs[dif][vmname]:
            if lower_dif not in neighsets:
                neighsets[lower_dif] = []
            neighsets[lower_dif].append(vmname)

    # Build the graph, represented as adjacency list
    for lower_dif in neighsets:
        # Each neighset corresponds to a complete (sub)graph.
        for vm1 in neighsets[lower_dif]:
            for vm2 in neighsets[lower_dif]:
                if vm1 != vm2:
                    dif_graphs[dif][vm1].append((vm2, lower_dif))

    enrollments[dif] = []
    # To generate the list of enrollments, we simulate one,
    # using breadth-first trasversal.
    enrolled = set([first])
    frontier = set([first])
    edges_covered = set()
    while len(frontier):
        cur = frontier.pop()
        for edge in dif_graphs[dif][cur]:
            if edge[0] not in enrolled:
                enrolled.add(edge[0])
                edges_covered.add((edge[0], cur))
                enrollments[dif].append({'enrollee': edge[0],
                                         'enroller': cur,
                                         'lower_dif': edge[1]})
                frontier.add(edge[0])

    lowerflowallocs[dif] = []
    if args.enrollment_strategy == 'full-mesh':
        for cur in dif_graphs[dif]:
            for edge in dif_graphs[dif][cur]:
                if cur < edge[0]:
                    if (cur, edge[0]) not in edges_covered and \
                            (edge[0], cur) not in edges_covered:
                        lowerflowallocs[dif].append({'enrollee': cur,
                                                     'enroller': edge[0],
                                                     'lower_dif': edge[1]})
                        edges_covered.add((cur, edge[0]))

    #print(neighsets)
    #print(dif_graphs[dif])

shim_id = 1
for shim in shims:
    enrollments[shim] = []
    lowerflowallocs[shim] = []
    shims[shim]['id'] = shim_id
    shim_id += 1


###################### Generate UP script ########################
fout = open('up.sh', 'w')

outs =  '#!/bin/bash\n'             \
        '\n'                        \
        'set -x\n'                  \
        '\n';

for shim in sorted(shims):
    outs += 'sudo brctl addbr %(br)s\n'         \
            'sudo ip link set %(br)s up\n'      \
            '\n' % {'br': shim}

for l in sorted(links):
    shim, vm = l
    idx = len(vms[vm]['ports']) + 1
    tap = '%s.%02x' % (vm, idx)

    outs += 'sudo ip tuntap add mode tap name %(tap)s\n'    \
            'sudo ip link set %(tap)s up\n'                 \
            'sudo brctl addif %(br)s %(tap)s\n\n'           \
                % {'tap': tap, 'br': shim}

    if shims[shim]['type'] == 'eth' and shims[shim]['speed'] > 0:
        speed = '%d%sbit' % (shims[shim]['speed'], shims[shim]['speed_unit'])
        # Rate limit the traffic transmitted on the TAP interface
        outs += 'sudo tc qdisc add dev %(tap)s handle 1: root '     \
                                'htb default 11\n'                  \
                'sudo tc class add dev %(tap)s parent 1: classid '  \
                                '1:1 htb rate 10gbit\n'             \
                'sudo tc class add dev %(tap)s parent 1:1 classid ' \
                                '1:11 htb rate %(speed)s\n'         \
                % {'tap': tap, 'speed': speed}

    vms[vm]['ports'].append({'tap': tap, 'shim': shim, 'idx': idx,
                             'ip': dns_mappings[shim][vm]['ip'] if shim in dns_mappings else None})


vmid = 1
budget = boot_batch_size

for vmname in sorted(vms):
    vm = vms[vmname]

    vm['id'] = vmid

    fwdp = args.base_port + vmid
    fwdc = fwdp + 10000
    mac = '00:0a:0a:0a:%02x:%02x' % (vmid, 99)

    vm['ssh'] = fwdp

    vars_dict = {'fwdp': fwdp, 'id': vmid, 'mac': mac,
                 'vmimgpath': vmimgpath, 'fwdc': fwdc,
                 'memory': args.memory, 'frontend': args.frontend}

    #'-serial tcp:127.0.0.1:%(fwdc)s,server,nowait '         \
    outs += 'qemu-system-x86_64 '
    outs += '-kernel buildroot/bzImage '                        \
            '-append "console=ttyS0" '                          \
            '-initrd %(vmimgpath)s '                            \
            '-nographic '                                       \
            '-display none '                                    \
            '--enable-kvm '                                     \
            '-smp 1 '                                           \
            '-m %(memory)sM '                                   \
            '-device %(frontend)s,mac=%(mac)s,netdev=mgmt '     \
            '-netdev user,id=mgmt,hostfwd=tcp::%(fwdp)s-:22 '   \
            '-vga std '                                         \
            '-pidfile rina-%(id)s.pid '                         \
                        % vars_dict

    del vars_dict

    for port in vm['ports']:
        tap = port['tap']
        mac = '00:0a:0a:0a:%02x:%02x' % (vmid, port['idx'])
        port['mac'] = mac

        outs += ''                                                      \
        '-device %(frontend)s,mac=%(mac)s,netdev=data%(idx)s '          \
        '-netdev tap,ifname=%(tap)s,id=data%(idx)s,script=no,'          \
        'downscript=no%(vhost)s '\
            % {'mac': mac, 'tap': tap, 'idx': port['idx'],
               'frontend': args.frontend,
               'vhost': ',vhost=on' if args.vhost else ''}

    outs += '&\n'

    budget -= 1
    if budget <= 0:
        outs += 'sleep %s\n' % wait_for_boot
        budget = boot_batch_size

    vmid += 1

# Compute DNS mappings
for vmname in sorted(vms):
    vm = vms[vmname]
    for dif in dif_ordering:
        if dif in shims or vmname not in difs[dif]:
            continue

        # Scan all the lower DIFs of the current DIF, for the current node
        for lower_dif in difs[dif][vmname]:
            if lower_dif in shims and shims[lower_dif]['type'] == 'udp4':
                vars_dict = {'dif': dif, 'id': vm['id']}
                dns_mappings[lower_dif][vmname]['name'] = '%(dif)s.%(id)s.IPCP-%(id)s--' % vars_dict
                del vars_dict


# Generate per-VM setup script
for vmname in sorted(vms):
    vm = vms[vmname]

    outs += ''\
            'DONE=255\n'\
            'while [ $DONE != "0" ]; do\n'\
            '   ssh %(sshopts)s -p %(ssh)s %(username)s@localhost << \'ENDSSH\'\n'\
                    'set -x\n'\
                    'SUDO=%(sudo)s\n'\
                    '$SUDO hostname %(name)s\n'\
                    '\n'\
            '\n' % {'name': vm['name'], 'ssh': vm['ssh'], 'username': username,
                    'sshopts': sshopts, 'sudo': sudo}

    verbmap = {'QUIET': 1, 'WARN': 2, 'INFO': 3, 'DBG': 4, 'VERY': 5}

    # Load kernel modules
    outs +=         '$SUDO modprobe rlite verbosity=%(verbidx)s\n'\
                    '$SUDO modprobe rlite-shim-eth\n'\
                    '$SUDO modprobe rlite-shim-udp4\n'\
                    '$SUDO modprobe rlite-normal\n'\
                    '$SUDO chmod a+rwx /dev/rlite\n'\
                    '$SUDO chmod a+rwx /dev/rlite-io\n'\
                    '$SUDO mkdir -p /var/rlite\n'\
                    '$SUDO chmod -R a+rw /var/rlite\n'\
                    '\n'\
                    '$SUDO rlite-uipcps -v %(verb)s &> uipcp.log &\n'\
                        % {'verb': args.verbosity,
                           'verbidx': verbmap[args.verbosity]}

    # Create and configure shim IPCPs
    for port in vm['ports']:
        shim = shims[port['shim']]
        vars_dict = {'mac': port['mac'], 'idx': port['idx'],
                     'shim': port['shim'], 'id': vm['id'],
                     'shimtype': shim['type']}
        outs +=     'PORT=$(mac2ifname %(mac)s)\n'\
                    '$SUDO ip link set $PORT up\n'\
                    '$SUDO rlite-ctl ipcp-create %(shim)s.%(id)s.IPCP/%(idx)s// shim-%(shimtype)s %(shim)s.DIF\n'\
                    % vars_dict
        if shim['type'] == 'eth':
                outs += '$SUDO rlite-ctl ipcp-config %(shim)s.%(id)s.IPCP/%(idx)s// netdev $PORT\n'\
                % vars_dict
        elif shim['type'] == 'udp4':
                outs += '$SUDO ip addr add %s dev $PORT\n' % (port['ip'])
        del vars_dict

    # Create normal IPCPs
    for dif in difs:
        if vmname in difs[dif]:
            outs += '$SUDO rlite-ctl ipcp-create %(dif)s.%(id)s.IPCP/%(id)s// normal %(dif)s.DIF\n'\
                    '$SUDO rlite-ctl ipcp-config %(dif)s.%(id)s.IPCP/%(id)s// address %(id)d\n'\
                        % {'dif': dif, 'id': vm['id']}

    # Update /etc/hosts file with DIF mappings
    for sh in dns_mappings:
        for nm in dns_mappings[sh]:
            outs += 'echo "%(ip)s %(name)s" >> /etc/hosts\n' \
                    % {'ip': prefix_prune_size(dns_mappings[sh][nm]['ip']), 'name': dns_mappings[sh][nm]['name']}

    # Carry out registrations following the DIF ordering
    for dif in dif_ordering:
        if dif in shims:
            # Shims don't register to other IPCPs
            continue

        if vmname not in difs[dif]:
            # Current node does not partecipate into the current DIF
            continue

        # Scan all the lower DIFs of the current DIF, for the current node
        for lower_dif in difs[dif][vmname]:
            vars_dict = {'dif': dif, 'id': vm['id'], 'lodif': lower_dif}
            outs += '$SUDO rlite-ctl ipcp-register %(lodif)s.DIF %(dif)s.%(id)s.IPCP/%(id)s//\n'\
                        % vars_dict
            del vars_dict

    outs +=         '\n'\
                    'sleep 1\n'\
                    'true\n'\
                'ENDSSH\n'\
            '   DONE=$?\n'\
            '   if [ $DONE != "0" ]; then\n'\
            '       sleep 1\n'\
            '   fi\n'\
            'done\n\n' % {'vmname': vm['name']}

if len(dns_mappings) > 0:
    print("DNS mappings: %s" % (dns_mappings))


# Run the enrollment operations in an order which respect the dependencies
for dif in dif_ordering:
    enrollments_list = enrollments[dif] + lowerflowallocs[dif]
    for enrollment in enrollments_list:
        vm = vms[enrollment['enrollee']]

        if enrollment in lowerflowallocs[dif]:
            oper = 'lower-flow-alloc'
        else:
            oper = 'enroll'

        print("%s %s to DIF %s against neighbor %s, through "\
                "lower DIF %s" % (oper, enrollment['enrollee'], dif,
                                  enrollment['enroller'],
                                  enrollment['lower_dif']))

        outs += 'sleep 1\n' # important!!
        outs += ''\
            'DONE=255\n'\
            'while [ $DONE != "0" ]; do\n'\
            '   ssh %(sshopts)s -p %(ssh)s %(username)s@localhost << \'ENDSSH\'\n'\
            'set -x\n'\
            'SUDO=%(sudo)s\n'\
            '$SUDO rlite-ctl ipcp-%(oper)s %(dif)s.DIF %(dif)s.%(id)s.IPCP/%(id)s// '\
                            '%(dif)s.%(pvid)s.IPCP/%(pvid)s// %(ldif)s.DIF\n'\
            'sleep 1\n'\
            'true\n'\
            'ENDSSH\n'\
            '   DONE=$?\n'\
            '   if [ $DONE != "0" ]; then\n'\
            '       sleep 1\n'\
            '   fi\n'\
            'done\n\n' % {'ssh': vm['ssh'], 'id': vm['id'],
                          'pvid': vms[enrollment['enroller']]['id'],
                          'username': username,
                          'vmname': vm['name'],
                          'dif': dif, 'ldif': enrollment['lower_dif'],
                          'sshopts': sshopts, 'sudo': sudo,
                          'oper': oper}

fout.write(outs)

fout.close()

subprocess.call(['chmod', '+x', 'up.sh'])


###################### Generate DOWN script ########################
fout = open('down.sh', 'w')

outs =  '#!/bin/bash\n'             \
        '\n'                        \
        'set -x\n'                  \
        '\n'                        \
        'kill_qemu() {\n'           \
        '   PIDFILE=$1\n'           \
        '   PID=$(cat $PIDFILE)\n'  \
        '   if [ -n $PID ]; then\n' \
        '       kill $PID\n'        \
        '       while [ -n "$(ps -p $PID -o comm=)" ]; do\n'    \
        '           sleep 1\n'                                  \
        '       done\n'                                         \
        '   fi\n'                                               \
        '\n'                                                    \
        '   rm $PIDFILE\n'                                      \
        '}\n\n'

for vmname in sorted(vms):
    vm = vms[vmname]
    outs += 'kill_qemu rina-%(id)s.pid\n' % {'id': vm['id']}

outs += '\n'

for vmname in sorted(vms):
    vm = vms[vmname]
    for port in vm['ports']:
        tap = port['tap']
        shim = port['shim']

        outs += 'sudo brctl delif %(br)s %(tap)s\n'             \
                'sudo ip link set %(tap)s down\n'               \
                'sudo ip tuntap del mode tap name %(tap)s\n\n'  \
                    % {'tap': tap, 'br': shim}

for shim in sorted(shims):
    outs += 'sudo ip link set %(br)s down\n'        \
            'sudo brctl delbr %(br)s\n'             \
            '\n' % {'br': shim}

fout.write(outs)

fout.close()

subprocess.call(['chmod', '+x', 'down.sh'])


# Dump the mapping from nodes to SSH ports
fout = open('demo.map', 'w')
for vmname in sorted(vms):
    fout.write('%s %d\n' % (vmname, args.base_port + vms[vmname]['id']))
fout.close()


if args.graphviz:
    try:
        import pydot

        colors = ['red', 'green', 'blue', 'orange', 'yellow']
        fcolors = ['black', 'black', 'white', 'black', 'black']

        gvizg = pydot.Dot(graph_type = 'graph')
        i = 0
        for dif in difs:
            for vmname in dif_graphs[dif]:
                node = pydot.Node(dif + vmname,
                                  label = "%s(%s)" % (vmname, dif),
                                  style = "filled", fillcolor = colors[i],
                                  fontcolor = fcolors[i])
                gvizg.add_node(node)

            for vmname in dif_graphs[dif]:
                for (neigh, lower_dif) in dif_graphs[dif][vmname]:
                    if vmname > neigh:
                        # Use lexicographical filter to avoid duplicate edges
                        continue
                    edge = pydot.Edge(dif + vmname, dif + neigh,
                                      label = lower_dif)
                    gvizg.add_edge(edge)

            i += 1
            if i == len(colors):
                i = 0

        gvizg.write_png('difs.png')
    except:
        print("Warning: pydot module not installed, cannot produce DIF "\
              "graphs images")

