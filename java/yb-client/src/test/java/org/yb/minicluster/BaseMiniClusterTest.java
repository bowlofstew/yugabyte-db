/**
 * Copyright (c) YugaByte, Inc.
 */
package org.yb.minicluster;

import com.google.common.net.HostAndPort;
import org.junit.AfterClass;
import org.junit.Before;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.yb.BaseYBTest;
import org.yb.client.MiniYBCluster;
import org.yb.client.MiniYBClusterBuilder;

import java.util.List;

import static org.junit.Assert.fail;

/**
 * A base class for tests using a MiniCluster.
 */
public class BaseMiniClusterTest extends BaseYBTest {

  private static final Logger LOG = LoggerFactory.getLogger(BaseMiniClusterTest.class);

  protected static final String NUM_MASTERS_PROP = "NUM_MASTERS";
  protected static final int NUM_TABLET_SERVERS = 3;
  protected static final int DEFAULT_NUM_MASTERS = 3;

  // Number of masters that will be started for this test if we're starting
  // a cluster.
  protected static final int NUM_MASTERS =
      Integer.getInteger(NUM_MASTERS_PROP, DEFAULT_NUM_MASTERS);

  /** This is used as the default timeout when calling YB Java client's async API. */
  protected static final int DEFAULT_SLEEP = 50000;

  /** A mini-cluster shared between invocations of multiple test methods. */
  protected static MiniYBCluster miniCluster;

  protected static List<String> masterArgs = null;
  protected static List<String> tserverArgs = null;

  // Comma separate describing the master addresses and ports.
  protected static String masterAddresses;
  protected static List<HostAndPort> masterHostPorts;

  /**
   * This makes sure that the mini cluster is up and running before each test. A test might opt to
   * leave the mini cluster running, and it will be reused by next tests, or it might shut down the
   * mini cluster by calling {@link BaseMiniClusterTest#destroyMiniCluster()}, and a new cluster
   * will be created for the next test.
   *
   * Even though {@link BaseMiniClusterTest#miniCluster} is a static variable, this logic is
   * implemented using {@link Before} and not {@link org.junit.BeforeClass}, because we need to know
   * the test class name so we can pass it as a command line parameter to master / tserver daemons
   * so we can better identify stuck processes.
   */
  @Before
  public void setUpBefore() throws Exception {
    if (miniCluster != null) {
      return;
    }

    miniCluster = new MiniYBClusterBuilder()
        .numMasters(NUM_MASTERS)
        .numTservers(NUM_TABLET_SERVERS)
        .defaultTimeoutMs(DEFAULT_SLEEP)
        .testClassName(getClass().getName())
        .masterArgs(masterArgs)
        .tserverArgs(tserverArgs)
        .build();
    masterAddresses = miniCluster.getMasterAddresses();
    masterHostPorts = miniCluster.getMasterHostPorts();

    LOG.info("Waiting for tablet servers...");
    if (!miniCluster.waitForTabletServers(NUM_TABLET_SERVERS)) {
      fail("Couldn't get " + NUM_TABLET_SERVERS + " tablet servers running, aborting");
    }

    afterStartingMiniCluster();
  }

  /**
   * This is called every time right after starting a mini cluster.
   */
  protected void afterStartingMiniCluster() throws Exception {
  }

  protected static void destroyMiniCluster() throws Exception {
    if (miniCluster != null) {
      LOG.info("Destroying mini cluster");
      miniCluster.shutdown();
      miniCluster = null;
    }
  }

  @AfterClass
  public static void tearDownAfterClass() throws Exception {
    LOG.info("BaseMiniClusterTest.tearDownAfterClass is running");
    destroyMiniCluster();
  }


}