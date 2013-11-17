package discovery;

import java.net.InetAddress;
import java.text.Collator;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.TreeSet;

import topo.GuiConnection;
import topo.GuiNode;

import com.cisco.onep.core.exception.OnepException;
import com.cisco.onep.core.util.OnepConstants;
import com.cisco.onep.element.NetworkApplication;
import com.cisco.onep.element.NetworkElement;
import com.cisco.onep.element.SessionConfig;
import com.cisco.onep.element.SessionHandle;
import com.cisco.onep.element.SessionConfig.SessionTransportMode;
import com.cisco.onep.interfaces.InterfaceFilter;
import com.cisco.onep.interfaces.NetworkInterface;
import com.cisco.onep.topology.Edge;
import com.cisco.onep.topology.Graph;
import com.cisco.onep.topology.Node;
import com.cisco.onep.topology.Topology;
import com.cisco.onep.topology.Topology.TopologyType;

public class NetworkDiscovery {

	public Graph 				graph 					= null;
	public List<InetAddress> 	addresses;
	public NetworkApplication 	discoveryApplication 	= NetworkApplication.getInstance();
	public SessionConfig 		nodeConfig;		
	public Collection<String> 	nodeNames 				= new TreeSet<String>(Collator.getInstance());
	public ArrayList<String> 	connectionStrings 		= new ArrayList<String>();
	public ArrayList<String>	paths 					= new ArrayList<String>();
	private String 				newLine 				= System.getProperty("line.separator");
	
	
	/**
	 * Discovers the network from the startAddress and generates a graph object
	 * 
	 * @param startAddress 	the InetAddress of the start node
	 * @param username		the username for all devices
	 * @param password		the password for all devices
	 */
	public NetworkDiscovery(InetAddress startAddress, InetAddress destAddress, String username, String password){
		
		//TODO: Testing code (keeping track of startTime)
		long startTime = System.nanoTime();		
		
		try {
			/*Initialize the global variables*/
			initalizeGlobals();
			
			/*Set up the Start Node*/
			NetworkElement node = discoveryApplication.getNetworkElement(startAddress);			
			
			/*Run recursive network discovery*/
			getNeighbors(node, username, password);
			
			/*Find paths*/
			findPaths(startAddress, destAddress);
			/*Print out the Global graph*/
			printNetwork();
			
		} catch (Exception e) {
			e.printStackTrace();
		}
		
		/*TODO: Testing code for timing the program execution time*/
		long endTime = System.nanoTime();
		System.out.println("Total Program took: " + ((endTime - startTime)/(Math.pow(10, 9))) + " seconds (" + (endTime - startTime) + ") ns");
		
		return;	
	}
	
	/** This will do what??
	 * 
	 * @param node				The network element we are checking
	 * @param username			The username for the device
	 * @param password			The password for the device
	 * @return
	 * @throws OnepException
	 */
	private Graph getNeighbors(NetworkElement node, String username, String password) throws OnepException{
		
		/*Connect to the node */		
		@SuppressWarnings("unused")
		SessionHandle nodeSession = node.connect(username, password, this.nodeConfig);
		
		/* Add all of the local IP addresses to our master list */
		List <NetworkInterface> interfaceList = node.getInterfaceList(new InterfaceFilter());
		
		for(NetworkInterface inter : interfaceList){
			if(inter.getAddressList().size() > 0 ){
				this.addresses.add(inter.getAddressList().get(0));
			}
		}
		
		/*Get Topology object */
		Topology topology = new Topology(node, TopologyType.CDP);
		
		/*If this is the first node, we set out global graph equal to this graph*/
		if(this.graph == null){
			this.graph = topology.getGraph();
		}
		
		/* Check all of the neighbors to see if we've been there yet*/
		List<Edge> edges = topology.getGraph().getEdgeList(Edge.EdgeType.UNDIRECTED);
		
		for(Edge edge: edges){
			
			InetAddress neighborIP = edge.getTailNodeConnector().getAddressList().get(0);
			/* If we haven't seen this IP address before, we haven't been to this device, we need to go to it*/
			if(!addressInNetwork(neighborIP)){
				/*Connect to neighbor and run recursive discovery*/
				NetworkElement daughterNode = discoveryApplication.getNetworkElement(neighborIP);
				Graph daughterGraph = getNeighbors(daughterNode, username, password);
				
				/*Disconnect from the daughterNode NetworkElement since we are done*/
				daughterNode.disconnect();
				
				/*Add the graph we got from the neighbor to the global graph*/
				this.graph.concatenate(daughterGraph);
			}
		}
		/*Return our graph (not our neighbors)*/
		return topology.getGraph();
	}
		
	/**
	 * Checks to see if the IP address is in our master list of addresses
	 * @param address - We check to see if this address in the global blacklist of addresses
	 * @return True if we already have seen this address
	 */
	private boolean addressInNetwork(InetAddress address){
		
		if(this.addresses.contains(address)){
			return true;
		}
		return false;
		
	}
	
	private void printNetwork() throws Exception{
		
		System.out.println("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
		System.out.println("Network Summary");
		System.out.println("~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~");
		
		
		/*Get a list of the device names*/
		List<Node>  nodes = this.graph.getNodeList();

		for(Node device: nodes){
			GuiNode node = new GuiNode(device.getName(), device.getName());
			//System.out.println(node.getNode());
			this.nodeNames.add(node.getNode());
		}
		
		/*Print out all of the connections*/
		List<Edge> edges = this.graph.getEdgeList(Edge.EdgeType.UNDIRECTED);
		
		int pktLoss = 0;
		for(Edge edge: edges){
			pktLoss+=10;
			GuiConnection connection = new GuiConnection(edge.getHeadNode().getName(),
														 shortenName(edge.getHeadNodeConnector().getName()),
														 edge.getTailNode().getName(),
														 shortenName(edge.getTailNodeConnector().getName()),
														 pktLoss);
			//System.out.println(connection.getConnection());
			this.connectionStrings.add(connection.getConnection());
		}
		
		

		
		//Generate JSON for GUI
 		//generateJson();
	}
	

	/**
	 * This will initialize the global variables for the NetworkDiscovery class
	 */
	private void initalizeGlobals(){
		this.nodeConfig = new SessionConfig(SessionTransportMode.SOCKET);
		this.nodeConfig.setPort(OnepConstants.ONEP_PORT);
		this.addresses = new ArrayList<InetAddress>();
	}

/**
 * Prepare topology for the JavaFX Gui
 */
	public void convertTopology(){
		
		System.out.println("elements: { " + newLine +
						   "	nodes: [");
		/*Prepare nodes */
		int i =1;
		for(String node : this.nodeNames){
			if(i++ < this.nodeNames.size()){
				System.out.println("		" + node + ",");
			}else{
				System.out.println("		" + node);
			}
		}
		
		System.out.println("	]," + newLine +
						   "	edges: [");
		
		/* Prepare connections */
		int j = 1;
		for(String connection: this.connectionStrings){
			if(j++ < this.connectionStrings.size()){
				System.out.println("		" +connection + ",");
			}else{
				System.out.println("		" +connection);
			}
		}
		
		System.out.println("	]" + newLine + 
						   "  },");
	}
	
	/**
	 * This will find all the paths from start to destination
	 * @param start - IP address of the start node
	 * @param dest  - IP address of the destination node
	 */
	private void findPaths(InetAddress start, InetAddress dest){
		//Print all possible routes
		Node startNode 	= null;
		Node destNode 	= null;
		List<Edge> edges = this.graph.getEdgeList(Edge.EdgeType.DIRECTED);
		for(Edge edge: edges){
			
			if(edge.getHeadNodeConnector().getAddressList().contains(start)){
				startNode = edge.getHeadNode();
			}else if(edge.getHeadNodeConnector().getAddressList().contains(dest)){
				destNode = edge.getHeadNode();
			}
		}
		System.out.println("Routes from " + startNode.getName() + " to " + destNode.getName());
		PathDiscovery pathTrace = new PathDiscovery(this.graph, startNode, destNode);
		pathTrace.getPaths();
	}
	
	/**
	 * Shortens the interface name from "GigabitEthernet0/0" -> "Gig0/0" etc
	 * @param longName
	 * @return shortened name
	 */
	private String shortenName(String longName){
		
		if(longName.startsWith("Eth")){
			longName = "Eth"+longName.substring(7);
		}else if(longName.startsWith("Fas")){
			longName = "Fa"+longName.substring(11);
		}else if(longName.startsWith("Gig")){
			longName = "Gi"+longName.substring(14);
		}else if(longName.startsWith("Ten")){
			longName = "Ten"+longName.substring(17);
		}
		return longName;

		}
}