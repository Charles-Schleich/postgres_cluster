use strict;
use warnings;
use Cluster;
use TestLib;
use Test::More tests => 9;

my $cluster = new Cluster(3);
$cluster->init();
$cluster->configure();
$cluster->start();

###############################################################################
# Wait until nodes are up
###############################################################################

my $ret;
my $psql_out;
# XXX: create extension on start and poll_untill status is Online
sleep(10);

###############################################################################
# Replication check
###############################################################################

$cluster->psql(0, 'postgres', "
	create extension multimaster;
	create table if not exists t(k int primary key, v int);
	insert into t values(1, 10);");
$cluster->psql(1, 'postgres', "select v from t where k=1;", stdout => \$psql_out);
is($psql_out, '10', "Check replication while all nodes are up.");

###############################################################################
# Isolation regress checks
###############################################################################

# we can call pg_regress here

###############################################################################
# Work after node stop
###############################################################################

note("stopping node 2");
if ($cluster->stopid(2, 'fast')) {
	pass("node 2 stops in fast mode");
} else {
	my $name = $cluster->{nodes}->[2]->name;
	$cluster->bail_out_with_logs("failed to stop $name in fast mode");
}

sleep(5); # Wait until failure of node will be detected

note("inserting 2 on node 0");
$ret = $cluster->psql(0, 'postgres', "insert into t values(2, 20);"); # this transaciton may fail
note  "tx1 status = $ret";
 

note("inserting 3 on node 1");
$ret = $cluster->psql(1, 'postgres', "insert into t values(3, 30);"); # this transaciton may fail
note("tx2 status = $ret");

note("inserting 4 on node 0 (can fail)");
$ret = $cluster->psql(0, 'postgres', "insert into t values(4, 40);"); 
note "tx1 status = $ret";

note("inserting 5 on node 1 (can fail)");
$ret = $cluster->psql(1, 'postgres', "insert into t values(5, 50);"); 
note("tx2 status = $ret");

note("selecting");
$cluster->psql(1, 'postgres', "select v from t where k=4;", stdout => \$psql_out);
note("selected");
is($psql_out, '40', "Check replication after node failure.");

###############################################################################
# Work after node start
###############################################################################

note("starting node 2");
$cluster->{nodes}->[2]->start;

sleep(5); # Wait until node is started

note("inserting 6 on node 0 (can fail)");
$cluster->psql(0, 'postgres', "insert into t values(6, 60);"); 
note("inserting 7 on node 1 (can fail)");
$cluster->psql(1, 'postgres', "insert into t values(7, 70);");

note("polling node 2");
for (my $poller = 0; $poller < 3; $poller++) {
	my $pollee = 2;
	ok($cluster->poll($poller, 'postgres', $pollee, 10, 1), "node $pollee is online according to node $poller");
}

note("getting cluster state");
$cluster->psql(0, 'postgres', "select * from mtm.get_cluster_state();", stdout => \$psql_out);
note("Node 1 status: $psql_out");
$cluster->psql(1, 'postgres', "select * from mtm.get_cluster_state();", stdout => \$psql_out);
note("Node 2 status: $psql_out");
$cluster->psql(2, 'postgres', "select * from mtm.get_cluster_state();", stdout => \$psql_out);
note("Node 3 status: $psql_out");

note("inserting 8 on node 0");
$cluster->psql(0, 'postgres', "insert into t values(8, 80);");
note("inserting 9 on node 1");
$cluster->psql(1, 'postgres', "insert into t values(9, 90);");

note("selecting from node 2");
$cluster->psql(2, 'postgres', "select v from t where k=8;", stdout => \$psql_out);
note("selected");

is($psql_out, '80', "Check replication after failed node recovery.");

$cluster->psql(2, 'postgres', "select v from t where k=9;", stdout => \$psql_out);
note("selected");

is($psql_out, '90', "Check replication after failed node recovery.");

ok($cluster->stop('fast'), "cluster stops");
1;
