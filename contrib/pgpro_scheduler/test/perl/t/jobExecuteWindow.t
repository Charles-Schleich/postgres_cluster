#!/usr/bin/perl
use strict;
no warnings;
use Test::More;
use DBI;
use Getopt::Long;

my $dbh = require 't/_connect.pl';
ok($dbh->err == 0) or (print $DBI::errstr and BAIL_OUT);

my $query = "DELETE FROM test_results;";
$dbh->do($query);
ok($dbh->err == 0) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);

$query = "SELECT schedule.create_job(NULL, '');";
my $sth = $dbh->prepare($query);
$sth->execute();
ok($dbh->err == 0) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);
my $job_id = $sth->fetchrow_array() and $sth->finish();

$query = "SELECT schedule.set_job_attribute(?, 'start_date', to_char(now() + interval \'2 minute\',
            \'YYYY-MM-DD HH24:MI\'))";
$sth = $dbh->prepare($query);
$sth->bind_param(1, $job_id);
ok($sth->execute()) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);
$sth->finish();

$query = "SELECT schedule.set_job_attribute(?, 'end_date', to_char(now() + interval \'4 minute\',
            \'YYYY-MM-DD HH24:MI\'))";
$sth = $dbh->prepare($query);
$sth->bind_param(1, $job_id);
ok($sth->execute()) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);
$sth->finish();

$query = "SELECT schedule.set_job_attributes(?, \'{ \"name\": \"Test\",
    \"cron\": \"* * * * *\",
    \"commands\": [\"INSERT INTO test_results (time_mark, commentary) VALUES(now(), ''jobExecuteWindow'')\",
                    \"INSERT INTO test_results (time_mark, commentary) VALUES(now(), ''jobExecuteWindow'')\"],
    \"run_as\": \"tester\",
    \"use_same_transaction\": \"true\"
    }\')";
$sth = $dbh->prepare($query);
$sth->bind_param(1, $job_id);
ok($sth->execute()) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);
$sth->finish();


sleep 60;
$query = "SELECT count(*) FROM test_results";
$sth = $dbh->prepare($query);
ok($sth->execute()) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);

my $result = $sth->fetchrow_array() and $sth->finish();
ok ($result == 0) or print "Worng row count != 0, count = $result\n";

sleep 300;
$query = "SELECT count(*) FROM test_results";
$sth = $dbh->prepare($query);
ok($sth->execute()) or (print $DBI::errstr and $dbh->disconnect() and BAIL_OUT);

my $result = $sth->fetchrow_array() and $sth->finish();
ok ($result == 2) or print "Worng row count != 2, count = $result\n";

$query = "SELECT schedule.deactivate_job(?)";
$sth = $dbh->prepare($query);
$sth->bind_param(1, $job_id);
ok($sth->execute()) or print $DBI::errstr ;
$sth->finish();

$query = "DELETE FROM test_results;";
$dbh->do($query);
ok($dbh->err == 0) or print $DBI::errstr;

$query = "SELECT schedule.drop_job(?)";
$sth = $dbh->prepare($query);
$sth->bind_param(1, $job_id);
ok($sth->execute()) or print $DBI::errstr;
$sth->finish();

$dbh->disconnect();

done_testing();
